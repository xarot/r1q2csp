# summarise a player prediction shot log
# usage: awk -f shot_stats.awk file.log

function val(key,   i, k) {
	k = key "="
	for (i = 1; i <= NF; i++)
		if (index($i, k) == 1)
			return substr($i, length(k) + 1)
	return ""
}

function vec(key, out,   i, k) {
	k = key "="
	for (i = 1; i <= NF; i++)
		if (index($i, k) == 1) {
			out[0] = substr($i, length(k) + 1) + 0
			out[1] = $(i + 1) + 0
			out[2] = $(i + 2) + 0
			return 1
		}
	return 0
}

function abs(x) { return x < 0 ? -x : x }

function add(g, err, along, ms, rawerr, rawalong, lerperr, lerpms) {
	n[g]++
	sErr[g] += err; sAlong[g] += along; sMs[g] += ms; sAbsMs[g] += abs(ms)
	sRaw[g] += rawerr; sRawAlong[g] += rawalong
	sLerp[g] += lerperr; sLerpMs[g] += lerpms
	if (err < 16) in16[g]++
	if (rawerr < 16) raw16[g]++
	if (lerperr < 16) lerp16[g]++
}

/^SHOT/ {
	click = val("click"); first = 1; mz = val("mz")
	next
}

/^ TGT/ {
	if (!first) next
	first = 0
	if (val("drawn") == "none") next

	vec("true", T); vec("drawn", D); vec("raw", R); vec("vel", V)
	speed = sqrt(V[0]*V[0] + V[1]*V[1])

	err = val("err") + 0; along = val("along") + 0; ms = val("ms") + 0
	lerperr = val("lerp_err") + 0; lerpms = val("lerp_ms") + 0

	rx = R[0]-T[0]; ry = R[1]-T[1]; rz = R[2]-T[2]
	rawerr = sqrt(rx*rx + ry*ry + rz*rz)
	rawalong = speed > 1 ? (rx*V[0] + ry*V[1]) / speed : 0

	zerr = D[2]-T[2]

	if (speed < 100) { add("still", err, along, ms, rawerr, rawalong, lerperr, lerpms); next }

	add("moving", err, along, ms, rawerr, rawalong, lerperr, lerpms)
	add(click == 1 ? "moving click" : "moving held", err, along, ms, rawerr, rawalong, lerperr, lerpms)

	fl = val("flags") + 0
	add(fl == 1 ? "moving ground" : (fl == 0 ? "moving air" : "moving other"), err, along, ms, rawerr, rawalong, lerperr, lerpms)

	sZ += abs(zerr); nZ++

	# histogram of along error in ms
	b = int((ms + 1000) / 25) * 25 - 1000
	hist[b]++
}

END {
	printf "%-14s %5s | %8s %8s %8s %8s %6s | %8s %8s %6s | %8s %8s %6s\n", "group", "n", "err", "along", "ms", "|ms|", "<16", "raw err", "raw alg", "<16", "lerp err", "lerp ms", "<16"
	split("moving,moving held,moving click,moving ground,moving air,moving other,still", gs, ",")
	for (i = 1; i <= 7; i++) {
		g = gs[i]
		if (!n[g]) continue
		printf "%-14s %5d | %8.1f %+8.1f %+8.0f %8.0f %5.0f%% | %8.1f %+8.1f %5.0f%% | %8.1f %+8.0f %5.0f%%\n", g, n[g],
			sErr[g]/n[g], sAlong[g]/n[g], sMs[g]/n[g], sAbsMs[g]/n[g], 100*in16[g]/n[g],
			sRaw[g]/n[g], sRawAlong[g]/n[g], 100*raw16[g]/n[g],
			sLerp[g]/n[g], sLerpMs[g]/n[g], 100*lerp16[g]/n[g]
	}
	if (nZ) printf "mean |z error| moving: %.1f\n", sZ/nZ
	print "along-error histogram (ms, moving targets):"
	for (b = -400; b <= 400; b += 25)
		if (hist[b]) printf "  %+5d..%+5d  %d\n", b, b + 25, hist[b]
}
