# Replays every player's logged movement (TRK lines) and scores forecast
# models against where the player really was H seconds later.
#
# usage: awk -v H=0.25 -f pptrk_replay.awk file.log

function val(key,   i, k) {
	k = key "="
	for (i = 1; i <= NF; i++)
		if (index($i, k) == 1)
			return substr($i, length(k) + 1)
	return ""
}

function abs(x) { return x < 0 ? -x : x }
function norm(a) { while (a > 180) a -= 360; while (a < -180) a += 360; return a }

function score(m, fx, fy, tx, ty,   e) {
	e = sqrt((fx-tx)*(fx-tx) + (fy-ty)*(fy-ty))
	sum[m, grp] += e; if (e < 16) ok[m, grp]++
	sum[m, "all"] += e; if (e < 16) ok[m, "all"]++
}

/^TRK/ {
	f = val("f") + 0; e = val("e") + 0
	for (i = 1; i <= NF; i++) {
		if (index($i, "o=") == 1) { X[e, f] = substr($i, 3) + 0; Y[e, f] = $(i+1) + 0; Z[e, f] = $(i+2) + 0 }
		if (index($i, "a=") == 1) { YAW[e, f] = $(i+1) + 0 }
	}
	FL[e, f] = val("fl") + 0
	have[e, f] = 1; ents[e] = 1
	if (f > maxf) maxf = f
	if (!minf || f < minf) minf = f
}

END {
	if (!H) H = 0.25
	k = H * 10; lo = int(k); fr = k - lo
	D2R = 0.0174533

	for (e in ents) for (f = minf; f <= maxf; f++) {
		if (!(have[e, f-2] && have[e, f-1] && have[e, f] && have[e, f+lo] && have[e, f+lo+1])) continue
		ok2 = 1
		for (j = f; j <= f + lo; j++) { if (!have[e, j]) ok2 = 0; if (sqrt((X[e, j+1]-X[e, j])^2 + (Y[e, j+1]-Y[e, j])^2) > 120) ok2 = 0 }
		if (!ok2) continue

		v1x = (X[e, f] - X[e, f-1]) * 10; v1y = (Y[e, f] - Y[e, f-1]) * 10
		v0x = (X[e, f-1] - X[e, f-2]) * 10; v0y = (Y[e, f-1] - Y[e, f-2]) * 10
		s1 = sqrt(v1x*v1x + v1y*v1y); s0 = sqrt(v0x*v0x + v0y*v0y)
		if (s1 > 1500 || s0 > 1500) continue

		tx = X[e, f+lo] + (X[e, f+lo+1] - X[e, f+lo]) * fr
		ty = Y[e, f+lo] + (Y[e, f+lo+1] - Y[e, f+lo]) * fr

		# what the player did afterwards, for grouping
		moved = sqrt((tx - X[e, f])^2 + (ty - Y[e, f])^2)

		if (s1 < 60) grp = (moved < 20 ? "standing" : "starting")
		else grp = (FL[e, f] == 1 ? "ground" : "air")

		n[grp]++; n["all"]++

		# 0: stock interpolation would show them about (H + 0.05) behind... not a forecast; skip
		# 1: straight line
		score(1, X[e, f] + v1x * H, Y[e, f] + v1y * H, tx, ty)

		# 2: facing: keep speed, but head where they now face plus the angle
		#    between facing and movement seen over the last interval (their keys)
		dyaw = norm(YAW[e, f] - YAW[e, f-1])
		if (s1 >= 60) {
			vdir = atan2(v1y, v1x) / D2R
			ymid = YAW[e, f-1] + dyaw * 0.5
			phi = norm(vdir - ymid)
			d = (YAW[e, f] + phi) * D2R
			score(2, X[e, f] + cos(d) * s1 * H, Y[e, f] + sin(d) * s1 * H, tx, ty)

			# 3: the same on the ground only (no air control in Quake 2)
			if (FL[e, f] == 1) score(3, X[e, f] + cos(d) * s1 * H, Y[e, f] + sin(d) * s1 * H, tx, ty)
			else score(3, X[e, f] + v1x * H, Y[e, f] + v1y * H, tx, ty)

			# 4: ground only, and assume the view keeps turning for a short while
			if (FL[e, f] == 1) {
				w = dyaw * 10; if (abs(dyaw) > 60) w = 0
				px = X[e, f]; py = Y[e, f]; el = 0
				for (t = H; t > 0.0001; t -= dt) {
					dt = t < 0.025 ? t : 0.025
					el += dt
					a = (YAW[e, f] + phi + w * 0.1 * (1 - exp(-el / 0.1))) * D2R
					px += cos(a) * s1 * dt; py += sin(a) * s1 * dt
				}
				score(4, px, py, tx, ty)
			} else score(4, X[e, f] + v1x * H, Y[e, f] + v1y * H, tx, ty)

			# 5: ground: halfway between straight line and facing
			if (FL[e, f] == 1) score(5, X[e, f] + (v1x + cos(d) * s1) * 0.5 * H, Y[e, f] + (v1y + sin(d) * s1) * 0.5 * H, tx, ty)
			else score(5, X[e, f] + v1x * H, Y[e, f] + v1y * H, tx, ty)
		} else {
			for (m = 2; m <= 5; m++) score(m, X[e, f], Y[e, f], tx, ty)
		}

		# 6: no forecast at all (latest known position)
		score(6, X[e, f], Y[e, f], tx, ty)
	}

	names[1] = "straight line (current)"; names[2] = "facing + keys"; names[3] = "facing + keys, ground only"
	names[4] = "ground facing + view turn"; names[5] = "ground: half straight, half facing"; names[6] = "no forecast (last position)"
	split("all ground air starting standing", gs, " ")

	printf "horizon %.0f ms\n%-36s", H*1000, "mean error / within 16 units"
	for (g = 1; g <= 5; g++) printf " %14s", gs[g] "(" n[gs[g]] ")"
	printf "\n"
	for (m = 1; m <= 6; m++) {
		printf "%-36s", names[m]
		for (g = 1; g <= 5; g++) if (n[gs[g]]) printf " %7.1f %4.0f%% ", sum[m, gs[g]] / n[gs[g]], 100 * ok[m, gs[g]] / n[gs[g]]; else printf " %14s", "-"
		printf "\n"
	}
}
