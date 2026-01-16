/**
 * @file pidcontroller.cpp
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

#include <cmath>

#include "pidcontroller.h"
#include "logger.h"

#ifdef PID_CONTROLLER_TUNING_AID_ADDRESS
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#endif

/**
 * Constructor for the PID Controller.
 *
 * @param kp            Proportional gain
 * @param ki            Integral gain
 * @param kd            Derivative gain
 * @param maxOutput     Max allowed output value (absolute)
 */
cPidController::cPidController(double kp, double ki, double kd, double maxOutput)
	: proportionalGain(kp),
	  integralGain(ki),
	  derivativeGain(kd),
	  maxOutput(maxOutput)
{
	// Calculate max integral so the I-term alone can never exceed the max output
	if (integralGain > 0)
		maxIntegral = maxOutput / integralGain;
	else
		maxIntegral = maxOutput;
}

/**
 * Calculates the new output value.
 *
 * @param currentValue     The current value
 * @param dt               The time elapsed since the last update (seconds)
 * @return The output value
 */
double cPidController::Update(double currentValue, double dt)
{
	if (dt <= 0.0)
		return 0.0;

	// Error > 0 means we are below target
	double error = targetValue - currentValue;

	pTerm = proportionalGain * error;

	if (!firstRun) { // the dt value is not yet valid on the first run
		integralSum += error * dt;

		// Anti-Windup: Clamp the integrator
		integralSum = std::min(integralSum, maxIntegral);
		integralSum = std::max(integralSum, -maxIntegral);

		iTerm = integralGain * integralSum;
		dTerm = derivativeGain * ((error - previousError) / dt);
	}

	double output = pTerm + iTerm + dTerm;

	previousError = error;
	firstRun = false;

	output = std::min(output, maxOutput);
	output = std::max(output, -maxOutput);

	if (std::abs(output) >= maxOutput) {
		LOGWARNING("pidcontroller: max output value exceeded. Resetting.");
		Reset();
	}
#ifdef PID_CONTROLLER_TUNING_AID_ADDRESS
	SendTuningAidData(pTerm, iTerm, dTerm, currentValue, output, targetValue);
#endif

	return output;
}

/**
 * Resets the internal state (integral sum and error history)
 */
void cPidController::Reset()
{
	firstRun = true;
	pTerm = 0;
	iTerm = 0;
	dTerm = 0;
	integralSum = 0.0;
	previousError = 0.0;
}

#ifdef PID_CONTROLLER_TUNING_AID_ADDRESS
/**
 * Live streams the internal PID controller state via UDP for visualization/tuning.
 *
 * This function is intended for debugging and tuning purposes only.
 * It sends a JSON payload containing P, I, D terms, input, output, and target values.
 *
 * To enable this feature:
 * 1. Define PID_CONTROLLER_TUNING_AID_ADDRESS at the top of this file.
 * 2. Update the IP address in the implementation to match your receiving
 *    machine running PlotJuggler with UDP server.
 */
void cPidController::SendTuningAidData(double pTerm, double iTerm, double dTerm, double input, double output, double targetValue)
{
	static int sock = -1;
	static struct sockaddr_in dest_addr;

	// One-time setup check
	if (sock < 0) {
		sock = socket(AF_INET, SOCK_DGRAM, 0);

		memset(&dest_addr, 0, sizeof(dest_addr));
		dest_addr.sin_family = AF_INET;
		dest_addr.sin_port = htons(9870); // PlotJuggler port
		inet_pton(AF_INET, PID_CONTROLLER_TUNING_AID_ADDRESS, &dest_addr.sin_addr); // replace with your PC's IP
	}

	// Actual Payload
	char payload[512];
	int len = snprintf(payload, sizeof(payload),
		"{\"bufferFillLevelMs\":%g,\"targetBufferFillLevelMs\":%g,\"pTerm\":%g,\"iTerm\":%g,\"dTerm\":%g,\"outputPpm\":%g}",
		input, targetValue, pTerm, iTerm, dTerm, output);

	// Send (Non-blocking usually, very fast)
	sendto(sock, payload, len, 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
}
#endif
