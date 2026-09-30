#!/bin/sh
# How far apart are two breakable street objects of the same model?
#
#   sh tools/mapobjects/measure.sh "C:/.../Grand Theft Auto 3"
#
# This is the measurement docs/objects.md 4 rests on, and it is a script rather
# than a paragraph for the same reason tools/sigmaker exists: the number it
# produces is the reason the identity tolerance is 0.25 m, and a reader should
# be able to re-derive it instead of trusting a document.
#
# What it does, in the engine's own terms:
#
#   - data/object.dat is the table CObjectData::Initialise parses. Column I is
#     the collision damage effect, and an object with effect 0 cannot break.
#   - an IPL `inst` line becomes a CDummyObject if and only if its model has an
#     object.dat entry (CFileLoader::LoadObjectInstance: `GetObjectID() == -1`
#     makes a CBuilding or a CTreadable instead), so joining the two files is
#     exactly the set of objects that can ever be a breakable CObject.
#   - the 13 IPLs are the ones gta3.dat actually loads, not everything in
#     data/maps - overview.IPL and props.IPL are in, several stray files are
#     not.
#
# Reads the game install and writes nothing. No part of the build needs it.

set -e

GAME="${1:-C:/Program Files (x86)/Steam/steamapps/common/Grand Theft Auto 3}"
DATA="$GAME/data"

if [ ! -f "$DATA/object.dat" ]; then
	echo "no object.dat under $DATA - pass the game directory as argument 1" >&2
	exit 1
fi

TMP="${TMPDIR:-/tmp}/coopiii-mapobjects.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# object.dat: name, then seven floats, then effect / response / camera-avoid.
# Commas and whitespace are both separators, ';' is a comment and '*' ends it.
awk '
	BEGIN { FS = "[ \t,]+" }
	{ sub(/\r$/, "") }
	/^[ \t]*;/ { next }
	/^[ \t]*\*/ { exit }
	{
		n = 0
		for (i = 1; i <= NF; i++) if ($i != "") f[++n] = $i
		if (n < 11) next
		# name  uproot  damageMult  effect  response  cameraAvoid
		printf "%s\t%s\t%s\t%s\t%s\t%s\n", tolower(f[1]), f[7], f[8], f[9], f[10], f[11]
	}
' "$DATA/object.dat" > "$TMP/objdat.tsv"

# Every `inst` line of every IPL gta3.dat loads.
: > "$TMP/inst.tsv"
for f in \
	COMNtop/COMNtop.IPL COMNbtm/COMNbtm.IPL COMSE/COMSE.IPL COMSW/COMSW.IPL \
	CULL.IPL INDUSTNE/INDUSTNE.IPL INDUSTNW/INDUSTNW.IPL INDUSTSE/INDUSTSE.IPL \
	INDUSTSW/INDUSTSW.IPL LANDne/LANDne.IPL LANDsw/LANDsw.IPL \
	overview.IPL props.IPL
do
	[ -f "$DATA/maps/$f" ] || { echo "missing $f" >&2; exit 1; }
	awk '
		BEGIN { FS = "[ \t]*,[ \t]*"; sec = "" }
		{ sub(/\r$/, ""); gsub(/^[ \t]+|[ \t]+$/, "") }
		/^#/ { next }
		$0 == "" { next }
		NF == 1 {
			low = tolower($1)
			if (low == "end") { sec = ""; next }
			sec = low
			next
		}
		sec == "inst" && NF >= 12 {
			printf "%s\t%s\t%s\t%s\t%s\n", $1, tolower($2), $3, $4, $5
		}
	' "$DATA/maps/$f" >> "$TMP/inst.tsv"
done

echo "map instances:            $(wc -l < "$TMP/inst.tsv")"

awk -F'\t' '
	NR == FNR { eff[$1] = $4; next }
	($2 in eff) { print $0 "\t" eff[$2] }
' "$TMP/objdat.tsv" "$TMP/inst.tsv" > "$TMP/objinst.tsv"

echo "of those, CDummyObjects:  $(wc -l < "$TMP/objinst.tsv")"
echo "of those, breakable:      $(awk -F'\t' '$6+0 != 0' "$TMP/objinst.tsv" | wc -l)"
echo

# Pairwise, inside each model index. The point of the whole script is the last
# three lines: the closest same-model pair anywhere in Liberty City has to stay
# clear of the tolerance in client/src/game/object.h.
awk -F'\t' '
	$6+0 != 0 {
		id = $1
		n[id]++
		X[id, n[id]] = $3 + 0
		Y[id, n[id]] = $4 + 0
		Z[id, n[id]] = $5 + 0
		NAME[id] = $2
	}
	END {
		globalmin = 1e18
		for (id in n) {
			if (n[id] < 2) continue
			m = 1e18; near2m = 0
			for (i = 1; i <= n[id]; i++)
				for (j = i + 1; j <= n[id]; j++) {
					dx = X[id,i] - X[id,j]
					dy = Y[id,i] - Y[id,j]
					dz = Z[id,i] - Z[id,j]
					d = sqrt(dx*dx + dy*dy + dz*dz)
					if (d < m) m = d
					if (d < 2.0)  { near2m++;  total2m++ }
					if (d < 0.5)  under05++
					if (d < 0.25) under025++
				}
			printf "%-20s id %-5s n=%-4d  closest pair %9.4f m   pairs<2m %d\n",
			       NAME[id], id, n[id], m, near2m
			if (m < globalmin) { globalmin = m; gname = NAME[id] }
		}
		printf "\nclosest same-model pair anywhere: %.4f m  (%s)\n", globalmin, gname
		printf "pairs closer than 2.00 m: %d\n", total2m + 0
		printf "pairs closer than 0.50 m: %d\n", under05 + 0
		printf "pairs closer than 0.25 m: %d\n", under025 + 0
	}
' "$TMP/objinst.tsv"
