/**
 * @file pidcontroller.h
 * Proportinal, Integral, Derivative Controller
 *
 * @license{AGPLv3
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.}
 */

#ifndef PID_CONTROLLER_H
#define PID_CONTROLLER_H

// Comment this in to transmit PID controller data for tuning/visualization. See DEVELOPER/README.md for instructions.
// #define PID_CONTROLLER_TUNING_AID_ADDRESS "192.168.2.22"

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
