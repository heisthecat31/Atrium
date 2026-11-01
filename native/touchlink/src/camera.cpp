// touchlink camera: TouchLink's calibration from the headset's own cameras (Valve Index, ...), kept
// right automatically while you play.
//
// The Index's front cameras see the Touches' IR LEDs (dimly, at normal exposure). Each camera frame
// comes from SteamVR with the headset's pose at its exposure, in SteamVR's raw space, the space the
// calibration maps the Touches into. So for every frame:
//   - each Touch's pose (grip, Touch space) at the frame's exposure comes from the tracking service;
//   - the current calibration puts it in raw space and projects it into each camera;
//   - the glow of its LEDs is looked for around there (the brightest pixels in a small window).
// Many such sightings, from different places and turns, fix the calibration: it's the rotation and
// offset (6 numbers) plus where the LED ring's glow sits on each Touch (3 per Touch) that best
// project the Touches onto what the cameras saw (Levenberg-Marquardt, outliers dropped). A better
// fit is written to calibration.json, which the driver reloads.
//
// It needs a first calibration to know where to look (touchlink calibrate, or Atrium's Calibrate),
// then keeps it right. Started by the driver when SteamVR starts; exits when SteamVR does.
// Writes camera.json (state, sightings, pixel error) in %APPDATA%\TouchLink for touchlink status
// and Atrium.
#include "calibration.h"
#include "source.h"

#include "openvr.h"

#include <algorithm>

namespace tl {
namespace {

// One sighting: a camera's pose (raw space, OpenVR's camera frame: -Z ahead, +Y up), the Touch's grip
// pose (Touch space), its intrinsics, and where the glow was (pixels in that camera's image).
struct Sighting {
	XrPosef camera;
	XrPosef touch;
	float fx, fy, cx, cy;
	float u, v;
	int hand;
};

// What's solved: the calibration (Touch space -> raw space) and the glow's offset on each Touch.
struct Params {
	XrPosef space = kIdentity;
	XrVector3f glow[2] = {};
};

constexpr size_t kKeep = 1500;            // sightings kept (the newest)
constexpr size_t kSolveEvery = 150;       // new sightings between solves
constexpr size_t kMinSolve = 200;         // before the first
constexpr float kMaxGlowOffset = 0.15f;   // the ring's glow is on the Touch

XrPosef FromVr(const vr::HmdMatrix34_t& m)
{
	double r[3][3];
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) r[i][j] = m.m[i][j];
	return { QFromMatrix(r), { m.m[0][3], m.m[1][3], m.m[2][3] } };
}

// Signs of the image axes against the camera frame (sx: +X right in the image; sy: +Y up is image
// up). Found from the data at the start, so a convention mix-up can't break it.
struct Projection { float sx = 1, sy = -1; };

bool Project(const Sighting& s, const Params& p, const Projection& pr, float* u, float* v)
{
	XrVector3f glow = QRot(s.touch.orientation, p.glow[s.hand]);
	XrVector3f inTouch = VAdd(s.touch.position, glow);
	XrVector3f raw = VAdd(p.space.position, QRot(p.space.orientation, inTouch));
	XrPosef inv = PInv(s.camera);
	XrVector3f c = VAdd(inv.position, QRot(inv.orientation, raw));
	if (c.z > -0.1f) return false;   // behind or too close
	*u = s.cx + pr.sx * s.fx * (c.x / -c.z);
	*v = s.cy - pr.sy * s.fy * (c.y / -c.z);
	return true;
}

// The parameters as 12 numbers around a base: a small turn and shift of the calibration, and the
// two glow offsets.
Params Apply(const Params& base, const double d[12])
{
	Params p = base;
	XrQuaternionf dq = FromEulerDeg((float)(d[0] * 57.29578), (float)(d[1] * 57.29578), (float)(d[2] * 57.29578));
	p.space.orientation = QMul(dq, base.space.orientation);
	p.space.position = VAdd(QRot(dq, base.space.position), { (float)d[3], (float)d[4], (float)d[5] });
	for (int h = 0; h < 2; h++) {
		p.glow[h] = VAdd(base.glow[h], { (float)d[6 + 3 * h], (float)d[7 + 3 * h], (float)d[8 + 3 * h] });
		float l = sqrtf(p.glow[h].x * p.glow[h].x + p.glow[h].y * p.glow[h].y + p.glow[h].z * p.glow[h].z);
		if (l > kMaxGlowOffset) p.glow[h] = { p.glow[h].x * kMaxGlowOffset / l, p.glow[h].y * kMaxGlowOffset / l, p.glow[h].z * kMaxGlowOffset / l };
	}
	return p;
}

// Gauss-Jordan on an n x n system (n <= 12). False if singular.
bool SolveSystem(int n, double A[12][13], double* x)
{
	for (int p = 0; p < n; p++) {
		int piv = p;
		for (int q = p + 1; q < n; q++)
			if (fabs(A[q][p]) > fabs(A[piv][p])) piv = q;
		if (fabs(A[piv][p]) < 1e-12) return false;
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

// Levenberg-Marquardt over the sightings (outliers past 3x the median dropped after the first
// rounds). Returns the median pixel error, or -1 if it couldn't fit.
double Fit(const std::vector<Sighting>& all, const Projection& pr, Params* p, size_t* used)
{
	std::vector<const Sighting*> s;
	for (const auto& x : all) s.push_back(&x);
	double lambda = 1e-3;
	double median = -1;
	// With Meta's source a level calibration (calibration.cpp fits only its yaw then) stays level: the
	// cameras correct its yaw and offset, not pitch or roll, which would tilt every Touch.
	const bool level = LoadConfig().meta && QRot(p->space.orientation, { 0, 1, 0 }).y > cosf(5 * 3.14159265f / 180);
	for (int iter = 0; iter < 25; iter++) {
		// Residuals at the current parameters; drop the far ones after a few rounds.
		std::vector<double> err;
		for (const Sighting* x : s) {
			float u, v;
			err.push_back(Project(*x, *p, pr, &u, &v) ? hypot(u - x->u, v - x->v) : 1e6);
		}
		std::vector<double> sorted = err;
		std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
		median = sorted[sorted.size() / 2];
		if (iter == 5 || iter == 12) {
			std::vector<const Sighting*> keep;
			double cut = std::max(3 * median, 4.0);
			for (size_t i = 0; i < s.size(); i++)
				if (err[i] < cut) keep.push_back(s[i]);
			if (keep.size() < kMinSolve / 2) return -1;
			s.swap(keep);
			continue;
		}
		// Normal equations from numeric derivatives.
		double JtJ[12][13] = {}, Jtr[12] = {};
		const double h[12] = { 1e-4, 1e-4, 1e-4, 1e-4, 1e-4, 1e-4, 1e-4, 1e-4, 1e-4, 1e-4, 1e-4, 1e-4 };
		double cost = 0;
		for (const Sighting* x : s) {
			float u0, v0;
			if (!Project(*x, *p, pr, &u0, &v0)) continue;
			double r[2] = { u0 - x->u, v0 - x->v };
			cost += r[0] * r[0] + r[1] * r[1];
			double J[2][12];
			for (int k = 0; k < 12; k++) {
				double d[12] = {};
				d[k] = h[k];
				Params q = Apply(*p, d);
				float u1, v1;
				if (!Project(*x, q, pr, &u1, &v1)) { u1 = u0; v1 = v0; }
				J[0][k] = (u1 - u0) / h[k];
				J[1][k] = (v1 - v0) / h[k];
				if (level && (k == 0 || k == 2)) J[0][k] = J[1][k] = 0;   // pitch, roll held
			}
			for (int a = 0; a < 12; a++) {
				for (int b = 0; b < 12; b++) JtJ[a][b] += J[0][a] * J[0][b] + J[1][a] * J[1][b];
				Jtr[a] += J[0][a] * r[0] + J[1][a] * r[1];
			}
		}
		double A[12][13];
		for (int a = 0; a < 12; a++) {
			for (int b = 0; b < 12; b++) A[a][b] = JtJ[a][b] + (a == b ? lambda * (JtJ[a][a] + 1e-6) : 0);
			A[a][12] = -Jtr[a];
			if (level && (a == 0 || a == 2)) A[a][a] = 1;   // held: d stays 0, and the system isn't singular
		}
		double d[12];
		if (!SolveSystem(12, A, d)) return -1;
		Params q = Apply(*p, d);
		double newCost = 0;
		for (const Sighting* x : s) {
			float u, v;
			if (Project(*x, q, pr, &u, &v)) newCost += (u - x->u) * (u - x->u) + (v - x->v) * (v - x->v);
		}
		if (newCost < cost) {
			*p = q;
			lambda = std::max(lambda / 3, 1e-7);
		} else {
			lambda *= 5;
		}
	}
	*used = s.size();
	return median;
}

// The centre of the glow around (u, v) in one camera's image (luminance, w x h, stride bytes per
// row, 4 bytes per pixel RGBA). False if nothing stands out there.
bool FindGlow(const uint8_t* img, int w, int h, int stride, float u, float v, int radius, float* gu, float* gv)
{
	int x0 = std::max(0, (int)u - radius), x1 = std::min(w - 1, (int)u + radius);
	int y0 = std::max(0, (int)v - radius), y1 = std::min(h - 1, (int)v + radius);
	if (x1 - x0 < 8 || y1 - y0 < 8) return false;
	auto lum = [&](int x, int y) {
		const uint8_t* p = img + y * stride + x * 4;
		return (int)p[0] + p[1] + p[2];   // 0..765
	};
	long sum = 0, n = 0;
	int peak = 0;
	for (int y = y0; y <= y1; y++)
		for (int x = x0; x <= x1; x++) {
			int l = lum(x, y);
			sum += l;
			n++;
			peak = std::max(peak, l);
		}
	double mean = (double)sum / n;
	if (peak - mean < 60) return false;   // nothing brighter than the background
	double thr = mean + 0.6 * (peak - mean);
	double wsum = 0, xs = 0, ys = 0;
	int count = 0;
	for (int y = y0; y <= y1; y++)
		for (int x = x0; x <= x1; x++) {
			double l = lum(x, y) - thr;
			if (l <= 0) continue;
			wsum += l;
			xs += l * x;
			ys += l * y;
			count++;
		}
	// A few LEDs make a small glow; a lamp or a window fills the window.
	if (count < 2 || count > (x1 - x0) * (y1 - y0) / 6) return false;
	*gu = (float)(xs / wsum);
	*gv = (float)(ys / wsum);
	return true;
}

// ---- the first calibration, from the two cameras alone --------------------------------------
// With two cameras, a glow both see is a 3D point in SteamVR's raw space (where their rays meet).
// Paired with where the Touches are (Touch space) at that moment, many such points fix the
// calibration with no starting guess: the one rigid turn and shift most pairs agree on (RANSAC
// over Horn fits). Wrong pairings (the other Touch, a lamp) are outliers. Each of the 4 image-axis
// conventions keeps its own pairs; the one that fits best is the convention.

struct Blob { float u, v; int peak; };

// The bright spots of one camera's image (at most 6, brightest first).
void FindBlobs(const uint8_t* img, int w, int h, int stride, std::vector<Blob>* out)
{
	out->clear();
	auto lum = [&](int x, int y) {
		const uint8_t* p = img + y * stride + x * 4;
		return (int)p[0] + p[1] + p[2];
	};
	double sum = 0, sq = 0;
	int n = 0;
	for (int y = 0; y < h; y += 4)
		for (int x = 0; x < w; x += 4) {
			int l = lum(x, y);
			sum += l;
			sq += (double)l * l;
			n++;
		}
	double mean = sum / n, sd = sqrt(std::max(0.0, sq / n - mean * mean));
	int thr = (int)(mean + std::max(5 * sd, 90.0));
	if (thr > 740) return;
	// Connected spots on a 2x grid (enough for a glow a few pixels across).
	int gw = w / 2, gh = h / 2;
	std::vector<int> label(gw * gh, -1);
	struct Acc { double u = 0, v = 0, wsum = 0; int count = 0, peak = 0; };
	std::vector<Acc> acc;
	std::vector<int> stack;
	for (int gy = 0; gy < gh; gy++)
		for (int gx = 0; gx < gw; gx++) {
			if (label[gy * gw + gx] != -1) continue;
			if (lum(gx * 2, gy * 2) <= thr) { label[gy * gw + gx] = -2; continue; }
			int id = (int)acc.size();
			acc.push_back({});
			stack.assign(1, gy * gw + gx);
			label[gy * gw + gx] = id;
			while (!stack.empty() && acc[id].count < 4000) {
				int i = stack.back();
				stack.pop_back();
				int x = (i % gw) * 2, y = (i / gw) * 2, l = lum(x, y);
				Acc& a = acc[id];
				double wgt = l - thr;
				a.u += wgt * x; a.v += wgt * y; a.wsum += wgt; a.count++;
				a.peak = std::max(a.peak, l);
				const int nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
				for (auto& d : nb) {
					int nx = i % gw + d[0], ny = i / gw + d[1];
					if (nx < 0 || ny < 0 || nx >= gw || ny >= gh || label[ny * gw + nx] != -1) continue;
					if (lum(nx * 2, ny * 2) <= thr) { label[ny * gw + nx] = -2; continue; }
					label[ny * gw + nx] = id;
					stack.push_back(ny * gw + nx);
				}
			}
		}
	for (const Acc& a : acc)
		if (a.count >= 1 && a.count <= 600 && a.wsum > 0) out->push_back({ (float)(a.u / a.wsum), (float)(a.v / a.wsum), a.peak });
	std::sort(out->begin(), out->end(), [](const Blob& a, const Blob& b) { return a.peak > b.peak; });
	if (out->size() > 6) out->resize(6);
}

// The ray through pixel (u, v) of a camera, in raw space.
void Ray(const XrPosef& cam, float fx, float fy, float cx, float cy, const Projection& pr, float u, float v, XrVector3f* o, XrVector3f* d)
{
	XrVector3f dc = { (u - cx) / (pr.sx * fx), (cy - v) / (pr.sy * fy), -1 };
	float l = sqrtf(dc.x * dc.x + dc.y * dc.y + dc.z * dc.z);
	dc = { dc.x / l, dc.y / l, dc.z / l };
	*o = cam.position;
	*d = QRot(cam.orientation, dc);
}

// Where two rays come closest: the midpoint, if they pass within `gap` in front of both cameras.
bool Meet(XrVector3f o1, XrVector3f d1, XrVector3f o2, XrVector3f d2, float gap, XrVector3f* p)
{
	XrVector3f w = VSub(o1, o2);
	float a = d1.x * d1.x + d1.y * d1.y + d1.z * d1.z, b = d1.x * d2.x + d1.y * d2.y + d1.z * d2.z,
		  c = d2.x * d2.x + d2.y * d2.y + d2.z * d2.z, d = d1.x * w.x + d1.y * w.y + d1.z * w.z,
		  e = d2.x * w.x + d2.y * w.y + d2.z * w.z, den = a * c - b * b;
	if (den < 1e-6f) return false;
	float s = (b * e - c * d) / den, t = (a * e - b * d) / den;
	if (s < 0.1f || t < 0.1f || s > 2.5f || t > 2.5f) return false;
	XrVector3f p1 = VAdd(o1, { d1.x * s, d1.y * s, d1.z * s }), p2 = VAdd(o2, { d2.x * t, d2.y * t, d2.z * t });
	XrVector3f g = VSub(p1, p2);
	if (sqrtf(g.x * g.x + g.y * g.y + g.z * g.z) > gap) return false;
	*p = { (p1.x + p2.x) / 2, (p1.y + p2.y) / 2, (p1.z + p2.z) / 2 };
	return true;
}

// Largest-eigenvalue eigenvector of a symmetric 4x4 (Jacobi), for Horn's method.
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
				for (int k = 0; k < 4; k++) { double x = a[k][p], y = a[k][q]; a[k][p] = c * x - s * y; a[k][q] = s * x + c * y; }
				for (int k = 0; k < 4; k++) { double x = a[p][k], y = a[q][k]; a[p][k] = c * x - s * y; a[q][k] = s * x + c * y; }
				for (int k = 0; k < 4; k++) { double x = v[k][p], y = v[k][q]; v[k][p] = c * x - s * y; v[k][q] = s * x + c * y; }
			}
	}
	int best = 0;
	for (int i = 1; i < 4; i++)
		if (a[i][i] > a[best][best]) best = i;
	for (int k = 0; k < 4; k++) out[k] = v[k][best];
}

struct PointPair { XrVector3f touch, raw; };

// The rigid pose taking Touch-space points onto raw-space points (Horn), over `use`.
bool HornFit(const std::vector<PointPair>& p, const std::vector<size_t>& use, XrPosef* out)
{
	if (use.size() < 3) return false;
	double ca[3] = {}, cb[3] = {};
	for (size_t i : use) {
		ca[0] += p[i].touch.x; ca[1] += p[i].touch.y; ca[2] += p[i].touch.z;
		cb[0] += p[i].raw.x; cb[1] += p[i].raw.y; cb[2] += p[i].raw.z;
	}
	for (int k = 0; k < 3; k++) { ca[k] /= use.size(); cb[k] /= use.size(); }
	double S[3][3] = {};
	for (size_t i : use) {
		double a[3] = { p[i].touch.x - ca[0], p[i].touch.y - ca[1], p[i].touch.z - ca[2] };
		double b[3] = { p[i].raw.x - cb[0], p[i].raw.y - cb[1], p[i].raw.z - cb[2] };
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++) S[r][c] += a[r] * b[c];
	}
	double N[4][4] = {
		{ S[0][0] + S[1][1] + S[2][2], S[1][2] - S[2][1], S[2][0] - S[0][2], S[0][1] - S[1][0] },
		{ S[1][2] - S[2][1], S[0][0] - S[1][1] - S[2][2], S[0][1] + S[1][0], S[2][0] + S[0][2] },
		{ S[2][0] - S[0][2], S[0][1] + S[1][0], -S[0][0] + S[1][1] - S[2][2], S[1][2] + S[2][1] },
		{ S[0][1] - S[1][0], S[2][0] + S[0][2], S[1][2] + S[2][1], -S[0][0] - S[1][1] + S[2][2] },
	};
	double e[4];
	Eigen4(N, e);
	double n = sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2] + e[3] * e[3]);
	if (!(n > 0)) return false;
	out->orientation = { (float)(e[1] / n), (float)(e[2] / n), (float)(e[3] / n), (float)(e[0] / n) };
	XrVector3f t = QRot(out->orientation, { (float)ca[0], (float)ca[1], (float)ca[2] });
	out->position = { (float)cb[0] - t.x, (float)cb[1] - t.y, (float)cb[2] - t.z };
	return true;
}

float PairError(const PointPair& p, const XrPosef& f)
{
	XrVector3f m = VAdd(f.position, QRot(f.orientation, p.touch));
	XrVector3f d = VSub(m, p.raw);
	return sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
}

// RANSAC over three-pair fits, then a fit on the agreeing pairs. Returns how many agree (0 if no fit).
size_t RansacFit(const std::vector<PointPair>& p, float inlier, XrPosef* out, float* rms)
{
	size_t n = p.size(), best = 0;
	if (n < 30) return 0;
	uint32_t seed = 4242;
	auto next = [&](size_t below) { seed = seed * 1664525u + 1013904223u; return (size_t)((seed >> 8) % below); };
	std::vector<size_t> bestIn;
	for (int iter = 0; iter < 2000; iter++) {
		std::vector<size_t> three = { next(n), next(n), next(n) };
		if (three[0] == three[1] || three[1] == three[2] || three[0] == three[2]) continue;
		XrPosef f;
		if (!HornFit(p, three, &f)) continue;
		std::vector<size_t> in;
		for (size_t i = 0; i < n; i++)
			if (PairError(p[i], f) < inlier) in.push_back(i);
		if (in.size() > bestIn.size()) bestIn = in;
	}
	XrPosef f;
	for (int round = 0; round < 2; round++) {
		if (!HornFit(p, bestIn, &f)) return 0;
		std::vector<size_t> in;
		for (size_t i = 0; i < n; i++)
			if (PairError(p[i], f) < inlier) in.push_back(i);
		bestIn = in;
	}
	double sum = 0;
	for (size_t i : bestIn) sum += PairError(p[i], f) * PairError(p[i], f);
	*rms = bestIn.empty() ? 1 : (float)sqrt(sum / bestIn.size());
	*out = f;
	best = bestIn.size();
	// The agreeing pairs must span some room, or the turn isn't pinned down.
	XrVector3f lo = { 1e9f, 1e9f, 1e9f }, hi = { -1e9f, -1e9f, -1e9f };
	for (size_t i : bestIn) {
		const XrVector3f& t = p[i].touch;
		lo = { std::min(lo.x, t.x), std::min(lo.y, t.y), std::min(lo.z, t.z) };
		hi = { std::max(hi.x, t.x), std::max(hi.y, t.y), std::max(hi.z, t.z) };
	}
	XrVector3f span = VSub(hi, lo);
	if (sqrtf(span.x * span.x + span.y * span.y + span.z * span.z) < 0.3f) return 0;
	return best;
}

void WriteState(const char* state, size_t sightings, double px, double mm, int updates)
{
	char buf[512];
	snprintf(buf, sizeof buf,
		"{\n  \"updated\": %lld,\n  \"state\": \"%s\",\n  \"sightings\": %zu,\n  \"error_px\": %.2f,\n  \"error_mm\": %.1f,\n"
		"  \"updates\": %d\n}\n",
		(long long)time(nullptr), state, sightings, px, mm, updates);
	WriteFileText(DataFile(L"camera.json"), buf);
}

} // namespace

int CameraMain()
{
	setvbuf(stdout, nullptr, _IONBF, 0);   // its log: driver-started, stdout is a file
	vr::EVRInitError err = vr::VRInitError_None;
	vr::IVRSystem* sys = vr::VR_Init(&err, vr::VRApplication_Background);
	if (!sys) {
		fprintf(stderr, "SteamVR isn't running (%d).\n", (int)err);
		return 1;
	}
	vr::IVRTrackedCamera* cam = vr::VRTrackedCamera();
	bool has = false;
	if (!cam || cam->HasCamera(vr::k_unTrackedDeviceIndex_Hmd, &has) != vr::VRTrackedCameraError_None || !has) {
		WriteState("no camera on this headset", 0, 0, 0, 0);
		vr::VR_Shutdown();
		return 0;
	}

	// The cameras: how the frame is laid out, where each sits on the headset, their intrinsics.
	vr::ETrackedPropertyError perr;
	int count = std::max(1, sys->GetInt32TrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_NumCameras_Int32, &perr));
	count = std::min(count, 2);
	int layout = sys->GetInt32TrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_CameraFrameLayout_Int32, &perr);
	vr::HmdMatrix34_t toHead[4] = {};
	sys->GetArrayTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_CameraToHeadTransforms_Matrix34_Array,
		vr::k_unHmdMatrix34PropertyTag, toHead, sizeof toHead, &perr);
	if (perr != vr::TrackedProp_Success) {
		toHead[0] = sys->GetMatrix34TrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_CameraToHeadTransform_Matrix34, &perr);
		count = 1;
	}
	const vr::EVRTrackedCameraFrameType type = vr::VRTrackedCameraFrameType_Undistorted;
	uint32_t fw = 0, fh = 0, fsize = 0;
	if (cam->GetCameraFrameSize(vr::k_unTrackedDeviceIndex_Hmd, type, &fw, &fh, &fsize) != vr::VRTrackedCameraError_None || !fsize) {
		WriteState("camera off: turn it on in SteamVR Settings > Camera, then restart SteamVR", 0, 0, 0, 0);
		vr::VR_Shutdown();
		return 0;
	}
	bool vertical = (layout & vr::EVRTrackedCameraFrameLayout_VerticalLayout) != 0;
	int sw = (int)fw, sh = (int)fh;   // one camera's image
	if (count == 2) {
		if (vertical) sh /= 2;
		else sw /= 2;
	}
	float fx[2], fy[2], cx[2], cy[2];
	for (int c = 0; c < count; c++) {
		vr::HmdVector2_t f = {}, ctr = {};
		cam->GetCameraIntrinsics(vr::k_unTrackedDeviceIndex_Hmd, c, type, &f, &ctr);
		fx[c] = f.v[0]; fy[c] = f.v[1]; cx[c] = ctr.v[0]; cy[c] = ctr.v[1];
	}
	cam->SetCameraTrackingSpace(vr::TrackingUniverseRawAndUncalibrated);
	vr::TrackedCameraHandle_t stream = INVALID_TRACKED_CAMERA_HANDLE;
	if (cam->AcquireVideoStreamingService(vr::k_unTrackedDeviceIndex_Hmd, &stream) != vr::VRTrackedCameraError_None) {
		WriteState("the camera is busy or off (SteamVR Settings > Camera)", 0, 0, 0, 0);
		vr::VR_Shutdown();
		return 0;
	}
	printf("Camera: %d x %d (%d camera%s, layout %d), %d x %d each\n", fw, fh, count, count > 1 ? "s" : "", layout, sw, sh);

	Source src;
	Config cfg = LoadConfig();
	std::wstring exe, manifest;
	std::vector<uint8_t> frame(fsize);
	std::vector<Sighting> sightings;
	Params params;
	Calibration cal = LoadCalibration();
	if (cal.valid) params.space = cal.space;
	Projection proj;
	// Until there's a calibration and the image axes are known: the first calibration from the two
	// cameras alone (point pairs for each of the 4 axis conventions), whatever calibration there is.
	bool projKnown = false;
	std::vector<PointPair> boot[4];
	size_t bootNew = 0;
	XrVector3f lastBoot[2] = { { 1e9f, 0, 0 }, { 1e9f, 0, 0 } };
	uint32_t lastSeq = 0;
	size_t sinceSolve = 0;
	int updates = 0;
	double lastPx = 0, lastMm = 0;
	ULONGLONG lastState = 0, lastTry = 0;

	for (;;) {
		vr::VREvent_t ev;
		while (sys->PollNextEvent(&ev, sizeof ev))
			if (ev.eventType == vr::VREvent_Quit) {
				cam->ReleaseVideoStreamingService(stream);
				vr::VR_Shutdown();
				return 0;
			}
		ULONGLONG now = GetTickCount64();
		if (!src.Ready() && now - lastTry > 3000) {
			lastTry = now;
			std::string why;
			if (cfg.meta) src.ConnectMeta(&why);   // the Touches from Meta's runtime (touchlink meta)
			else if (ServiceFiles(cfg.serviceDir, &exe, &manifest)) src.Connect(manifest, &why);
		}
		if (now - lastState > 1000) {
			lastState = now;
			const char* state = !src.Ready() ? "waiting for the Touches"
				: !projKnown ? (count < 2 ? "needs a first calibration (hold a Touch against the headset once)"
					: "finding the Touches with both cameras: move them around in front of you")
				: sightings.size() < kMinSolve ? "collecting sightings"
				: "keeping the calibration right";
			WriteState(state, sightings.size(), lastPx, lastMm, updates);
		}
		Sleep(15);
		if (!src.Ready()) continue;
		// One camera can't triangulate: it needs a calibration to start from.
		if (count < 2 && !cal.valid) {
			Calibration c = LoadCalibration();
			if (c.valid) { cal = c; params.space = c.space; projKnown = false; }
			continue;
		}

		vr::CameraVideoStreamFrameHeader_t hdr = {};
		if (cam->GetVideoStreamFrameBuffer(stream, type, frame.data(), fsize, &hdr, sizeof hdr) != vr::VRTrackedCameraError_None) continue;
		if (hdr.nFrameSequence == lastSeq || !hdr.trackedDevicePose.bPoseIsValid) continue;
		lastSeq = hdr.nFrameSequence;
		XrPosef head = FromVr(hdr.trackedDevicePose.mDeviceToAbsoluteTracking);
		int stride = (int)hdr.nWidth * (int)hdr.nBytesPerPixel;
		if (hdr.nBytesPerPixel != 4) continue;

		TouchPose tp[2];
		bool got[2];
		for (int h = 0; h < 2; h++) got[h] = src.Locate(h, (LONGLONG)hdr.ulFrameExposureTime, &tp[h]) && tp[h].positionTracked;

		// The first calibration (and the axes) from both cameras, no calibration needed.
		if (!projKnown && count == 2 && (got[0] || got[1])) {
			std::vector<Blob> blobs[2];
			XrPosef camPose[2];
			for (int c = 0; c < 2; c++) {
				camPose[c] = PMul(head, FromVr(toHead[c]));
				const uint8_t* img = frame.data() + (c == 1 ? (vertical ? sh * stride : sw * 4) : 0);
				FindBlobs(img, sw, sh, stride, &blobs[c]);
			}
			for (int k = 0; k < 4; k++) {
				Projection pr = { k & 1 ? -1.f : 1.f, k & 2 ? 1.f : -1.f };
				for (const Blob& a : blobs[0])
					for (const Blob& b : blobs[1]) {
						XrVector3f o1, d1, o2, d2, p;
						Ray(camPose[0], fx[0], fy[0], cx[0], cy[0], pr, a.u, a.v, &o1, &d1);
						Ray(camPose[1], fx[1], fy[1], cx[1], cy[1], pr, b.u, b.v, &o2, &d2);
						if (!Meet(o1, d1, o2, d2, 0.025f, &p)) continue;
						for (int h = 0; h < 2; h++)
							if (got[h]) boot[k].push_back({ tp[h].grip.position, p });
					}
				if (boot[k].size() > 3000) boot[k].erase(boot[k].begin(), boot[k].end() - 3000);
			}
			// Count new sightings only when a Touch has moved (a still one adds nothing).
			for (int h = 0; h < 2; h++) {
				if (!got[h]) continue;
				XrVector3f d = VSub(tp[h].grip.position, lastBoot[h]);
				if (sqrtf(d.x * d.x + d.y * d.y + d.z * d.z) > 0.02f) {
					lastBoot[h] = tp[h].grip.position;
					bootNew++;
				}
			}
			if (bootNew >= 150) {
				bootNew = 0;
				size_t bestCount = 0;
				int bestK = -1;
				XrPosef bestFit;
				float bestRms = 1;
				for (int k = 0; k < 4; k++) {
					XrPosef f;
					float rms;
					size_t in = RansacFit(boot[k], 0.08f, &f, &rms);
					printf("Camera start, axes %d: %zu of %zu pairs agree (%.1f cm)\n", k, in, boot[k].size(), rms * 100);
					if (in > bestCount) { bestCount = in; bestK = k; bestFit = f; bestRms = rms; }
				}
				if (bestK >= 0 && bestCount >= 60) {
					proj = { bestK & 1 ? -1.f : 1.f, bestK & 2 ? 1.f : -1.f };
					projKnown = true;
					params = Params();
					params.space = bestFit;
					Calibration c = cal;
					c.valid = true;
					c.space = bestFit;
					c.errorMm = bestRms * 1000;
					CalResult r;
					r.rmsMm = c.errorMm;
					SaveCalibration(c, r, "cameras");
					cal = c;
					updates++;
					sightings.clear();
					printf("Camera start: calibrated from %zu pairs (%.1f cm); refining from here\n", bestCount, bestRms * 100);
				}
			}
			continue;
		}
		if (!projKnown) continue;

		for (int c = 0; c < count; c++) {
			XrPosef camPose = PMul(head, FromVr(toHead[c]));
			const uint8_t* img = frame.data() + (count == 2 && c == 1 ? (vertical ? sh * stride : sw * 4) : 0);
			for (int h = 0; h < 2; h++) {
				if (!got[h]) continue;
				Sighting s = { camPose, tp[h].grip, fx[c], fy[c], cx[c], cy[c], 0, 0, h };
				float u, v;
				if (!Project(s, params, proj, &u, &v) || u < 0 || v < 0 || u >= sw || v >= sh) continue;
				// Both Touches close together in the image: which glow is which isn't clear.
				if (got[1 - h]) {
					Sighting o = s;
					o.touch = tp[1 - h].grip;
					o.hand = 1 - h;
					float ou, ov;
					if (Project(o, params, proj, &ou, &ov) && hypot(ou - u, ov - v) < 90) continue;
				}
				XrVector3f d = VSub(VAdd(params.space.position, QRot(params.space.orientation, tp[h].grip.position)), camPose.position);
				float dist = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
				int radius = std::max(25, std::min(90, (int)(fx[c] * 0.08f / std::max(dist, 0.2f))));
				if (!FindGlow(img, sw, sh, stride, u, v, radius, &s.u, &s.v)) continue;
				sightings.push_back(s);
				sinceSolve++;
			}
		}
		if (sightings.size() > kKeep) sightings.erase(sightings.begin(), sightings.end() - kKeep);
		if (sightings.size() < kMinSolve || sinceSolve < kSolveEvery) continue;
		sinceSolve = 0;

		Params fit = params;
		size_t used = 0;
		double px = Fit(sightings, proj, &fit, &used);
		if (px < 0 || px > 6 || used < kMinSolve / 2) {
			printf("Camera fit: no (%.1f px over %zu)\n", px, used);
			continue;
		}
		// The 3D error: a pixel at the typical distance.
		double dsum = 0;
		for (const auto& s : sightings) {
			XrVector3f d = VSub(VAdd(fit.space.position, QRot(fit.space.orientation, s.touch.position)), s.camera.position);
			dsum += sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
		}
		double mm = px / fx[0] * (dsum / sightings.size()) * 1000;
		lastPx = px;
		lastMm = mm;
		XrVector3f moved = VSub(fit.space.position, params.space.position);
		XrQuaternionf turn = QMul(fit.space.orientation, QConj(params.space.orientation));
		double movedMm = sqrt(moved.x * moved.x + moved.y * moved.y + moved.z * moved.z) * 1000;
		double turnedDeg = 2 * acos(std::min(1.0, (double)fabsf(turn.w))) * 57.29578;
		params = fit;
		printf("Camera fit: %.1f px (%.1f mm) over %zu sightings; moved %.1f mm, turned %.2f deg\n", px, mm, used, movedMm, turnedDeg);
		if (movedMm < 2 && turnedDeg < 0.2) continue;
		Calibration c = cal;
		c.space = fit.space;
		c.errorMm = mm;
		CalResult r;
		r.rmsMm = mm;
		SaveCalibration(c, r, "camera");
		cal = c;
		updates++;
	}
}

} // namespace tl
