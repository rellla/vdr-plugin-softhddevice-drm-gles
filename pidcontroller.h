// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file pidcontroller.h
 * PID (proportional, integral, derivative) Controller Header File
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef PID_CONTROLLER_H
#define PID_CONTROLLER_H

// Comment this in to transmit PID controller data for tuning/visualization. See DEVELOPER/README.md for instructions.
// #define PID_CONTROLLER_TUNING_AID_ADDRESS "192.168.2.22"

/**
 * PID Controller
 *
 * @ingroup audio
 */
class cPidController {
public:
	cPidController(double, double, double, double);
	double GetTargetValue() { return targetValue; }
	double GetPTerm() { return pTerm; }
	double GetITerm() { return iTerm; }
	double GetDTerm() { return dTerm; }
	void Reset();
	void SetTargetValue(double value) { targetValue = value; }
	double Update(double, double);

private:
	double proportionalGain = 0; ///< Proportional Gain (Kp) - Reaction strength
	double integralGain = 0;     ///< Integral Gain (Ki) - Drift correction
	double derivativeGain = 0;   ///< Derivative Gain (Kd) - Dampening

	double pTerm = 0;            ///< Proportional term
	double iTerm = 0;            ///< Integral term
	double dTerm = 0;            ///< Derivative term

	double targetValue = 0;      ///< The desired buffer fill level in frames
	double integralSum = 0;      ///< Accumulator for the I-term
	double previousError = 0;    ///< Error from the previous step (for D-term)

	bool firstRun = true;        ///< Flag for first run
	double maxOutput = 0;        ///< Hard limit for output correction
	double maxIntegral = 0;      ///< Anti-windup limit for the integral term

#ifdef PID_CONTROLLER_TUNING_AID_ADDRESS
	void SendTuningAidData(double, double, double, double, double, double);
#endif
};

#endif
