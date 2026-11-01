// The solve. The Touch is fixed to the headset, so for every moment t
//     C * touch(t) = hmd(t) * X
// with C the unknown turn and offset between the spaces and X where the Touch sits on the headset.
//   - Turn: hmd_j hmd_i^-1 = C (touch_j touch_i^-1) C^-1, whatever X is, so the rotation axes of
//     the headset's turns are the Touch's turned by C: Horn over many pairs (as touch.cpp).
//   - Offset: p_C + R_C t(t) = h(t) + R_hmd(t) x is linear in (p_C, x): least squares, which
//     needs the head turned about more than one axis.
//   - Delay: lighthouse and constellation poses don't arrive equally late; delays from -120 to
//     +120 ms are tried (the Touch moved by its velocities) and the best kept.
#include "calibration.h"

#include <algorithm>

namespace tl {
namespace {

// Largest-eigenvalue eigenvector of a symmetric 4x4 (Jacobi).
void Eigen4(double a[4][4], double out[4])
{
	double v[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
	for (int sweep = 0; sweep < 50; sweep++) {
		double off = 0;
		for (int p = 0; p < 4; p++)
			for (int q = p + 1; q < 4; q++) off += a[p][q] * a[p][q];
		if (off < 1e-18) break;
		for (int p = 0; p < 4; p++)
			for (int q = p + 1; q < 4; q++) {
				if (fabs(a[p][q]) < 1e-30) continue;
				double theta = (a[q][q] - a[p][p]) / (2 * a[p][q]);
				double t = (theta >= 0 ? 1 : -1) / (fabs(theta) + sqrt(theta * theta + 1));
				double c = 1 / sqrt(t * t + 1), s = t * c;
				for (int k = 0; k < 4; k++) {
					double akp = a[k][p], akq = a[k][q];
					a[k][p] = c * akp - s * akq;
					a[k][q] = s * akp + c * akq;
				}
				for (int k = 0; k < 4; k++) {
					double apk = a[p][k], aqk = a[q][k];
					a[p][k] = c * apk - s * aqk;
					a[q][k] = s * apk + c * aqk;
				}
				for (int k = 0; k < 4; k++) {
					double vkp = v[k][p], vkq = v[k][q];
					v[k][p] = c * vkp - s * vkq;
					v[k][q] = s * vkp + c * vkq;
				}
			}
	}
	int best = 0;
	for (int i = 1; i < 4; i++)
		if (a[i][i] > a[best][best]) best = i;
	for (int k = 0; k < 4; k++) out[k] = v[k][best];
}

XrVector3f RotVec(XrQuaternionf q)
{
	if (q.w < 0) q = { -q.x, -q.y, -q.z, -q.w };
	double s = sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
	if (s < 1e-9) return { 0, 0, 0 };
	double k = 2 * atan2(s, q.w) / s;
	return { (float)(q.x * k), (float)(q.y * k), (float)(q.z * k) };
}

XrQuaternionf FromRotVec(XrVector3f v)
{
	double a = sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
	if (a < 1e-9) return { 0, 0, 0, 1 };
	double s = sin(a / 2) / a;
	return { (float)(v.x * s), (float)(v.y * s), (float)(v.z * s), (float)cos(a / 2) };
}

double Angle(const XrQuaternionf& a, const XrQuaternionf& b)
{
	double d = fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
	return 2 * acos(fmin(1.0, d));
}

XrPosef Predict(const CalSample& s, double lag)
{
	XrPosef p = s.touch;
	p.orientation = QMul(FromRotVec({ (float)(s.angVel.x * lag), (float)(s.angVel.y * lag), (float)(s.angVel.z * lag) }), p.orientation);
	p.position = { (float)(p.position.x + s.linVel.x * lag), (float)(p.position.y + s.linVel.y * lag),
		(float)(p.position.z + s.linVel.z * lag) };
	return p;
}

bool SolveTurn(const std::vector<XrPosef>& touch, const std::vector<XrPosef>& hmd, bool level, XrQuaternionf* R)
{
	double S[3][3] = {};
	int pairs = 0;
	for (size_t stride : { 4, 8, 16, 32, 64, 128 })
		for (size_t i = 0; i + stride < hmd.size(); i++) {
			size_t j = i + stride;
			XrVector3f a = RotVec(QMul(hmd[j].orientation, QConj(hmd[i].orientation)));
			XrVector3f b = RotVec(QMul(touch[j].orientation, QConj(touch[i].orientation)));
			if (a.x * a.x + a.y * a.y + a.z * a.z < 0.25 * 0.25) continue;
			double av[3] = { a.x, a.y, a.z }, bv[3] = { b.x, b.y, b.z };
			for (int r = 0; r < 3; r++)
				for (int c = 0; c < 3; c++) S[r][c] += bv[r] * av[c];
			pairs++;
		}
	if (pairs < 20) return false;
	double N[4][4] = {
		{ S[0][0] + S[1][1] + S[2][2], S[1][2] - S[2][1], S[2][0] - S[0][2], S[0][1] - S[1][0] },
		{ S[1][2] - S[2][1], S[0][0] - S[1][1] - S[2][2], S[0][1] + S[1][0], S[2][0] + S[0][2] },
		{ S[2][0] - S[0][2], S[0][1] + S[1][0], -S[0][0] + S[1][1] - S[2][2], S[1][2] + S[2][1] },
		{ S[0][1] - S[1][0], S[2][0] + S[0][2], S[1][2] + S[2][1], -S[0][0] - S[1][1] + S[2][2] },
	};
	double e[4];
	Eigen4(N, e);
	double n = sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2] + e[3] * e[3]);
	*R = { (float)(e[1] / n), (float)(e[2] / n), (float)(e[3] / n), (float)(e[0] / n) };
	// With both spaces standing on gravity (SteamVR's raw space and Meta's floor-level one), the turn
	// between them is about the vertical only. Fitting all three axes let the head's pitch and roll
	// noise tilt every Touch by a few degrees; then only the yaw is fitted (the same pairs: maximise
	// sum a . R_y(t) b). The tracking service's space may lean (a sensor's), so it keeps the full
	// turn, and so does one that leans more than 5 degrees anyway.
	XrVector3f up = QRot(*R, { 0, 1, 0 });
	if (level && up.y > cos(5 * 3.14159265 / 180)) {
		double t = atan2(S[2][0] - S[0][2], S[0][0] + S[2][2]);
		*R = { 0, (float)sin(t / 2), 0, (float)cos(t / 2) };
	}
	return true;
}

// Gauss-Jordan with partial pivoting, n x (n+1). False if singular.
bool SolveLinear(int n, double A[6][7], double* x)
{
	for (int p = 0; p < n; p++) {
		int piv = p;
		for (int q = p + 1; q < n; q++)
			if (fabs(A[q][p]) > fabs(A[piv][p])) piv = q;
		if (fabs(A[piv][p]) < 1e-6) return false;
		for (int k = 0; k <= n; k++) std::swap(A[p][k], A[piv][k]);
		for (int q = 0; q < n; q++) {
			if (q == p) continue;
			double f = A[q][p] / A[p][p];
			for (int k = 0; k <= n; k++) A[q][k] -= f * A[p][k];
		}
	}
	for (int p = 0; p < n; p++) x[p] = A[p][n] / A[p][p];
	return true;
}

// One delay: C and X, and the leftover error (m). False when the movement can't pin them down.
bool SolveAt(const std::vector<CalSample>& track, double lag, bool level, CalResult* r)
{
	std::vector<XrPosef> touch(track.size()), hmd(track.size());
	for (size_t i = 0; i < track.size(); i++) {
		touch[i] = Predict(track[i], lag);
		hmd[i] = track[i].hmd;
	}
	XrQuaternionf Rc;
	if (!SolveTurn(touch, hmd, level, &Rc)) return false;

	double A[6][7] = {};
	for (size_t i = 0; i < track.size(); i++) {
		double R[3][3];
		QToMatrix(hmd[i].orientation, R);
		XrVector3f rt = QRot(Rc, touch[i].position);
		double b[3] = { hmd[i].position.x - rt.x, hmd[i].position.y - rt.y, hmd[i].position.z - rt.z };
		// Row block M = [I, -R]: M^T M = [[I, -R], [-R^T, I]], M^T b = [b, -R^T b].
		for (int r = 0; r < 3; r++) {
			A[r][r] += 1;
			A[r + 3][r + 3] += 1;
			for (int c = 0; c < 3; c++) {
				A[r][c + 3] -= R[r][c];
				A[c + 3][r] -= R[r][c];
				A[r + 3][6] -= R[c][r] * b[c];
			}
			A[r][6] += b[r];
		}
	}
	double u[6];
	if (!SolveLinear(6, A, u)) return false;
	XrVector3f pc = { (float)u[0], (float)u[1], (float)u[2] }, x = { (float)u[3], (float)u[4], (float)u[5] };

	double sum = 0;
	for (size_t i = 0; i < track.size(); i++) {
		XrVector3f l = VAdd(pc, QRot(Rc, touch[i].position));
		XrVector3f rr = VAdd(hmd[i].position, QRot(hmd[i].orientation, x));
		XrVector3f d = VSub(l, rr);
		sum += d.x * d.x + d.y * d.y + d.z * d.z;
	}
	r->space = { Rc, pc };
	r->onHead.position = x;
	r->rmsMm = sqrt(sum / track.size()) * 1000;
	r->lagMs = lag * 1000;
	return true;
}

} // namespace

CalResult Solve(const std::vector<CalSample>& track, bool level)
{
	CalResult best;
	if (track.size() < 150) {
		snprintf(best.why, sizeof best.why, "the Touch was tracked for too short a time (%zu samples; is a Rift sensor facing it?)",
			track.size());
		return best;
	}
	double turned = 0;
	for (const auto& s : track) turned = fmax(turned, Angle(s.hmd.orientation, track[0].hmd.orientation));
	best.turnedDeg = turned * 57.29578;
	if (best.turnedDeg < 40) {
		snprintf(best.why, sizeof best.why, "the head only turned %.0f degrees: look left, right, up and down, and tilt it",
			best.turnedDeg);
		return best;
	}

	bool any = false;
	auto tryLag = [&](double lag) {
		CalResult r;
		if (!SolveAt(track, lag, level, &r)) return;
		if (!any || r.rmsMm < best.rmsMm) {
			double t = best.turnedDeg;
			best = r;
			best.turnedDeg = t;
		}
		any = true;
	};
	for (int ms = -120; ms <= 120; ms += 10) tryLag(ms / 1000.0);
	if (!any) {
		snprintf(best.why, sizeof best.why, "the head didn't turn about enough different axes: tilt it sideways and nod too");
		return best;
	}
	double centre = best.lagMs;
	for (int ms = -8; ms <= 8; ms += 2) tryLag((centre + ms) / 1000.0);

	// How rigid the Touch stayed on the headset: its turn in the headset's frame should not change.
	// Measured from the average turn (one odd sample, the first say, mustn't decide it).
	std::vector<XrQuaternionf> on(track.size());
	double m[4] = {};
	for (size_t i = 0; i < track.size(); i++) {
		XrQuaternionf x = QMul(QConj(track[i].hmd.orientation), QMul(best.space.orientation, Predict(track[i], best.lagMs / 1000).orientation));
		if (x.x * on[0].x + x.y * on[0].y + x.z * on[0].z + x.w * on[0].w < 0 && i) x = { -x.x, -x.y, -x.z, -x.w };
		on[i] = x;
		m[0] += x.x, m[1] += x.y, m[2] += x.z, m[3] += x.w;
	}
	double n = sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2] + m[3] * m[3]);
	XrQuaternionf mean = { (float)(m[0] / n), (float)(m[1] / n), (float)(m[2] / n), (float)(m[3] / n) };
	std::vector<double> off(track.size());
	for (size_t i = 0; i < track.size(); i++) off[i] = Angle(on[i], mean);
	std::nth_element(off.begin(), off.begin() + off.size() * 9 / 10, off.end());
	best.spread = off[off.size() * 9 / 10] * 57.29578;   // 90% of the time within this
	best.onHead.orientation = mean;

	XrVector3f x = best.onHead.position;
	double reach = sqrt(x.x * x.x + x.y * x.y + x.z * x.z);
	if (best.spread > 6)
		snprintf(best.why, sizeof best.why, "the Touch moved on the headset (%.1f degrees): hold it still against the side", best.spread);
	else if (reach > 0.35)
		snprintf(best.why, sizeof best.why, "the Touch came out %.0f cm from the headset: hold it against the headset", reach * 100);
	else if (best.rmsMm > 20)
		snprintf(best.why, sizeof best.why, "the fit is off by %.0f mm (tracking dropped out?): try again, slower", best.rmsMm);
	else
		best.ok = true;
	return best;
}

Calibration LoadCalibration()
{
	Calibration c;
	std::string text;
	if (!ReadFileText(CalibrationFile(), &text)) return c;
	double q[4], p[3], v;
	if (JsonNumbers(text, "rotation", q, 4) != 4 || JsonNumbers(text, "translation", p, 3) != 3) return c;
	c.space = { { (float)q[0], (float)q[1], (float)q[2], (float)q[3] }, { (float)p[0], (float)p[1], (float)p[2] } };
	if (JsonNumber(text, "error_mm", &v)) c.errorMm = v;
	std::string u;
	if (JsonString(text, "universe", &u)) c.universe = _strtoui64(u.c_str(), nullptr, 10);
	c.valid = true;
	return c;
}

bool SaveCalibration(const Calibration& c, const CalResult& r, const char* hand)
{
	char buf[1024];
	snprintf(buf, sizeof buf,
		"{\n  \"rotation\": [%.9f, %.9f, %.9f, %.9f],\n  \"translation\": [%.6f, %.6f, %.6f],\n"
		"  \"error_mm\": %.2f,\n  \"lag_ms\": %.1f,\n  \"spread_deg\": %.2f,\n  \"turned_deg\": %.0f,\n"
		"  \"hand\": \"%s\",\n  \"universe\": \"%llu\",\n  \"when\": %lld\n}\n",
		c.space.orientation.x, c.space.orientation.y, c.space.orientation.z, c.space.orientation.w, c.space.position.x,
		c.space.position.y, c.space.position.z, r.rmsMm, r.lagMs, r.spread, r.turnedDeg, hand, (unsigned long long)c.universe,
		(long long)time(nullptr));
	return WriteFileText(CalibrationFile(), buf);
}

} // namespace tl
