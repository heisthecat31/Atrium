// calib_check: runs the calibration solve on synthetic tracks (tilted sensor space, a Touch held
// to the headset, the Touch 40 ms late, noise) and says how close it got. Not shipped.
#include "calibration.h"

using namespace tl;

namespace {
double Noise(double scale) { return ((rand() / (double)RAND_MAX) * 2 - 1) * scale; }

// The headset at time t: looking around (yaw, pitch, roll at different rates), swaying a bit.
XrPosef Head(double t)
{
	XrQuaternionf q = FromEulerDeg((float)(25 * sin(t * 1.3)), (float)(70 * sin(t * 0.7)), (float)(20 * sin(t * 1.9 + 1)));
	return { q, { (float)(0.1 * sin(t * 0.5)), (float)(1.7 + 0.05 * sin(t * 0.9)), (float)(0.08 * cos(t * 0.6)) } };
}
} // namespace

int main()
{
	srand(7);
	const XrPosef C = { FromEulerDeg(4, 123, -3), { 1.2f, -0.3f, 0.7f } };   // Touch space -> raw
	const XrPosef X = { FromEulerDeg(0, 90, 10), { 0.11f, 0.0f, 0.03f } };    // Touch on the headset
	const double late = 0.040, dt = 0.01;
	auto touchAt = [&](double t) { return PMul(PInv(C), PMul(Head(t), X)); };

	std::vector<CalSample> track;
	for (int i = 0; i < 1000; i++) {
		double t = 2 + i * dt;
		XrPosef a = touchAt(t - late), b = touchAt(t - late + 0.001);
		CalSample s;
		s.hmd = Head(t);
		s.touch = a;
		s.touch.position = VAdd(s.touch.position, { (float)Noise(0.002), (float)Noise(0.002), (float)Noise(0.002) });
		s.linVel = { (b.position.x - a.position.x) * 1000, (b.position.y - a.position.y) * 1000, (b.position.z - a.position.z) * 1000 };
		XrQuaternionf d = QMul(b.orientation, QConj(a.orientation));
		if (d.w < 0) d = { -d.x, -d.y, -d.z, -d.w };
		s.angVel = { d.x * 2000, d.y * 2000, d.z * 2000 };
		track.push_back(s);
	}
	CalResult r = Solve(track, false);
	XrQuaternionf e = QMul(r.space.orientation, QConj(C.orientation));
	double turnErr = 2 * acos(fmin(1.0, fabs(e.w))) * 57.29578;
	XrVector3f d = VSub(r.space.position, C.position);
	double posErr = sqrt(d.x * d.x + d.y * d.y + d.z * d.z) * 1000;
	printf("ok=%d why='%s'\nturn error %.3f deg, offset error %.2f mm, delay %.0f ms (true %.0f), fit %.2f mm, wobble %.2f deg, turned %.0f deg\n",
		r.ok, r.why, turnErr, posErr, r.lagMs, late * 1000, r.rmsMm, r.spread, r.turnedDeg);
	bool pass = r.ok && turnErr < 0.5 && posErr < 5 && fabs(r.lagMs - late * 1000) <= 4;

	// Too little head movement must be refused, not solved badly.
	std::vector<CalSample> still(track.begin(), track.begin() + 200);
	for (auto& s : still) s.hmd.orientation = Head(2).orientation;
	CalResult r2 = Solve(still, false);
	printf("still head: ok=%d why='%s'\n", r2.ok, r2.why);
	pass = pass && !r2.ok;

	// A Touch in a hand (moving on its own while the head looks around) must never fit: the driver
	// solves every window while it looks for a Touch held to the headset.
	std::vector<CalSample> hand = track;
	for (size_t i = 0; i < hand.size(); i++) {
		double t = 2 + i * dt;
		XrPosef inHand = { FromEulerDeg((float)(30 * sin(t * 2.1)), (float)(40 * sin(t * 1.1)), (float)(25 * sin(t * 1.7))),
			{ (float)(0.25 * sin(t * 0.8)), (float)(1.2 + 0.1 * sin(t * 1.4)), (float)(-0.3 + 0.1 * cos(t)) } };
		hand[i].touch = PMul(PInv(C), inHand);
		hand[i].angVel = hand[i].linVel = { 0, 0, 0 };
	}
	CalResult r3 = Solve(hand, false);
	printf("in hand: ok=%d why='%s'\n", r3.ok, r3.why);
	pass = pass && !r3.ok;

	// Meta's source: both spaces stand on gravity, the Touch's turn is noisy. The level solve must
	// come out with no tilt at all and the yaw right.
	const XrPosef L = { FromEulerDeg(0, -57, 0), { -0.4f, 0.02f, 1.1f } };
	std::vector<CalSample> lev = track;
	for (size_t i = 0; i < lev.size(); i++) {
		double t = 2 + i * dt;
		lev[i].touch = PMul(PInv(L), PMul(Head(t - late), X));
		lev[i].touch.orientation = QMul(FromEulerDeg((float)Noise(1.5), (float)Noise(1.5), (float)Noise(1.5)), lev[i].touch.orientation);
		lev[i].angVel = lev[i].linVel = { 0, 0, 0 };
	}
	CalResult r4 = Solve(lev, true);
	XrVector3f up = QRot(r4.space.orientation, { 0, 1, 0 });
	XrQuaternionf e4 = QMul(r4.space.orientation, QConj(L.orientation));
	double tilt4 = acos(fmin(1.0, up.y)) * 57.29578, turn4 = 2 * acos(fmin(1.0, fabs(e4.w))) * 57.29578;
	printf("level: ok=%d why='%s' tilt %.4f deg, turn error %.3f deg\n", r4.ok, r4.why, tilt4, turn4);
	pass = pass && r4.ok && tilt4 < 0.01 && turn4 < 0.5;
	printf(pass ? "PASS\n" : "FAIL\n");
	return pass ? 0 : 1;
}
