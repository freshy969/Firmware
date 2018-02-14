/****************************************************************************
 *
 *   Copyright (c) 2018 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file FlightAutoLine.cpp
 */

#include "FlightTaskAutoLine.hpp"
#include <mathlib/mathlib.h>

using namespace matrix;

#define SIGMA_SINGLE_OP			0.000001f
#define SIGMA_NORM			0.001f

FlightTaskAutoLine::FlightTaskAutoLine(control::SuperBlock *parent, const char *name) :
	FlightTaskAuto(parent, name),
	_land_speed(parent, "MPC_LAND_SPEED", false),
	_vel_max_up(parent, "MPC_Z_VEL_MAX_UP", false),
	_vel_max_down(parent, "MPC_Z_VEL_MAX_DN", false),
	_acc_max_xy(parent, "MPC_ACC_HOR_MAX", false),
	_acc_xy(parent, "MPC_ACC_HOR", false),
	_acc_max_up(parent, "MPC_ACC_UP_MAX", false),
	_acc_max_down(parent, "MPC_ACC_DOWN_MAX", false),
	_cruise_speed_90(parent, "MPC_CRUISE_90", false),
	_nav_rad(parent, "NAV_ACC_RAD", false),
	_mis_yaw_error(parent, "MIS_YAW_ERR", false)
{}

bool FlightTaskAutoLine::activate()
{
	_reset();
	return FlightTaskAuto::activate();
}

bool FlightTaskAutoLine::update()
{
	if (_type == WaypointType::idle) {
		_generateIdleSetpoints();

	} else if (_type == WaypointType::land) {
		_generateLandSetpoints();

	} else if (_type == WaypointType::loiter || _type == WaypointType::position) {
		_generateSetpoints();

	} else if (_type == WaypointType::takeoff) {
		_generateTakeoffSetpoints();

	} else if (_type == WaypointType::velocity) {
		_generateVelocitySetpoints();
	}

	/* For now yaw setpoint comes directly form triplets */
	_setYawSetpoint(_yaw_wp);

	return true;
}

void FlightTaskAutoLine::_reset()
{
	/* Set setpoints equal current state. */
	_vel_sp_xy = matrix::Vector2f(&_velocity(0));
	_pos_sp_xy = matrix::Vector2f(&_position(0));
	_vel_sp_z = _velocity(2);
	_pos_sp_z = _position(2);
	_destination = _target;
	_origin = _prev_wp;
	_speed_at_target = 0.0f;
}

void FlightTaskAutoLine::_generateIdleSetpoints()
{
	/* Send zero thrust setpoint */
	_setThrustSetpoint(Vector3f(0.0f, 0.0f, 0.0f));

	/* Set member setpoints to current state */
	_reset();
}

void FlightTaskAutoLine::_generateLandSetpoints()
{
	/* Keep xy-position and go down with landspeed. */
	_setPositionSetpoint(Vector3f(_target(0), _target(1), NAN));
	_setVelocitySetpoint(Vector3f(NAN, NAN, _land_speed.get()));

	/* Set member setpoints to current state */
	_reset();
}

void FlightTaskAutoLine::_generateTakeoffSetpoints()
{
	/* Takeoff is completely defined by position. */
	_setPositionSetpoint(_target);

	/* Set member setpoints to current state */
	_reset();
}

void FlightTaskAutoLine::_generateSetpoints()
{
	_updateInternalWaypoints();
	_generateAltitudeSetpoints();
	_generateXYsetpoints();
	_setPositionSetpoint(Vector3f(_pos_sp_xy(0), _pos_sp_xy(1), _pos_sp_z));
	_setVelocitySetpoint(Vector3f(_vel_sp_xy(0), _vel_sp_xy(1), _vel_sp_z));
}

void FlightTaskAutoLine::_updateInternalWaypoints()
{
	/* The internal Waypoints might differ from previous_wp and target. The cases where it differs:
	 * 1. The vehicle already passe the target -> go straight to target
	 * 2. The vehicle is more than cruise speed in front of previous waypoint -> go straight to previous wp
	 * 3. The vehicle is more than cruise speed from track -> go straight to closest point on track
	 *
	 * If a new target is available, then the speed at the target is computed from the angle previous-target-next
	 */

	/* Adjust destination and origin based on current vehicle state. */
	Vector2f u_prev_to_target = Vector2f(&(_target - _prev_wp)(0)).unit_or_zero();
	Vector2f pos_to_target = Vector2f(&(_target - _position)(0));
	Vector2f prev_to_pos = Vector2f(&(_position - _prev_wp)(0));
	Vector2f closest_pt = Vector2f(&_prev_wp(0)) + u_prev_to_target * (prev_to_pos * u_prev_to_target);

	if (u_prev_to_target * pos_to_target < 0.0f) {

		/* Target is behind. */
		if (_current_state != State::target_behind) {

			_destination = _target;
			_origin = _position;
			_current_state = State::target_behind;

			float angle = 2.0f;
			_speed_at_target = 0.0f;

			/* angle = cos(x) + 1.0
			 * angle goes from 0 to 2 with 0 = large angle, 2 = small angle:   0 = PI ; 2 = PI*0 */

			if (Vector2f(&(_destination - _next_wp)(0)).length() > 0.001f &&
			    (Vector2f(&(_destination - _origin)(0)).length() > _nav_rad.get())) {

				angle = Vector2f(&(_destination - _origin)(0)).unit_or_zero()
					* Vector2f(&(_destination - _next_wp)(0)).unit_or_zero()
					+ 1.0f;
				_speed_at_target = _getVelocityFromAngle(angle);
			}
		}

	} else if (u_prev_to_target * prev_to_pos < 0.0f && prev_to_pos.length() > _mc_cruise_speed) {

		/* Current position is more than cruise speed in front of previous setpoint. */
		if (_current_state != State::previous_infront) {
			_destination = _prev_wp;
			_origin = _position;
			_current_state = State::previous_infront;

			float angle = 2.0f;
			_speed_at_target = 0.0f;

			/* angle = cos(x) + 1.0
			 * angle goes from 0 to 2 with 0 = large angle, 2 = small angle:   0 = PI ; 2 = PI*0 */
			if (Vector2f(&(_destination - _next_wp)(0)).length() > 0.001f &&
			    (Vector2f(&(_destination - _origin)(0)).length() > _nav_rad.get())) {

				angle = Vector2f(&(_destination - _origin)(0)).unit_or_zero()
					* Vector2f(&(_destination - _next_wp)(0)).unit_or_zero()
					+ 1.0f;
				_speed_at_target = _getVelocityFromAngle(angle);
			}

		}

	} else if (Vector2f(Vector2f(&_position(0)) - closest_pt).length() > _mc_cruise_speed) {

		/* Vehicle is more than cruise speed off track. */
		if (_current_state != State::offtrack) {
			_destination = matrix::Vector3f(closest_pt(0), closest_pt(1), _target(2));
			_origin = _position;
			_current_state = State::offtrack;

			float angle = 2.0f;
			_speed_at_target = 0.0f;

			/* angle = cos(x) + 1.0
			 * angle goes from 0 to 2 with 0 = large angle, 2 = small angle:   0 = PI ; 2 = PI*0 */
			if (Vector2f(&(_destination - _next_wp)(0)).length() > 0.001f &&
			    (Vector2f(&(_destination - _origin)(0)).length() > _nav_rad.get())) {

				angle = Vector2f(&(_destination - _origin)(0)).unit_or_zero()
					* Vector2f(&(_destination - _next_wp)(0)).unit_or_zero()
					+ 1.0f;
				_speed_at_target = _getVelocityFromAngle(angle);
			}

		}

	} else {

		if ((_target - _destination).length() > 0.01f) {
			/* A new target is available. Update speed at target.*/
			_destination = _target;
			_origin = _prev_wp;
			_current_state = State::none;

			float angle = 2.0f;
			_speed_at_target = 0.0f;

			/* angle = cos(x) + 1.0
			 * angle goes from 0 to 2 with 0 = large angle, 2 = small angle:   0 = PI ; 2 = PI*0 */
			if (Vector2f(&(_destination - _next_wp)(0)).length() > 0.001f &&
			    (Vector2f(&(_destination - _origin)(0)).length() > _nav_rad.get())) {

				angle =
					Vector2f(&(_destination - _origin)(0)).unit_or_zero()
					* Vector2f(&(_destination - _next_wp)(0)).unit_or_zero()
					+ 1.0f;
				_speed_at_target = _getVelocityFromAngle(angle);
			}
		}
	}
}

void FlightTaskAutoLine::_generateXYsetpoints()
{
	Vector2f pos_sp_to_dest = Vector2f(&_target(0)) - _pos_sp_xy;
	const bool has_reached_altitude = fabsf(_destination(2) - _position(2)) < _nav_rad.get();

	if ((_speed_at_target < 0.001f && pos_sp_to_dest.length() < _nav_rad.get()) ||
	    (!has_reached_altitude && pos_sp_to_dest.length() < _nav_rad.get())) {

		/* Vehicle reached target in xy and no passing required. Lock position */
		_pos_sp_xy = Vector2f(&_destination(0));
		_vel_sp_xy.zero();

	} else {

		/* Get various path specific vectors. */
		Vector2f u_prev_to_dest = Vector2f(&(_destination - _origin)(0)).unit_or_zero();
		Vector2f prev_to_pos(&(_position - _origin)(0));
		Vector2f closest_pt = Vector2f(&_origin(0)) + u_prev_to_dest * (prev_to_pos * u_prev_to_dest);
		Vector2f closest_to_dest = Vector2f(&(_destination - _position)(0));
		Vector2f prev_to_dest = Vector2f(&(_destination - _origin)(0));
		float speed_sp_track = _mc_cruise_speed;
		float speed_sp_prev_track = math::max(_vel_sp_xy * u_prev_to_dest, 0.0f);

		/* Distance to target when brake should occur. The assumption is made that
		 * 1.5 * cruising speed is enough to break. */
		float target_threshold = 1.5f * _mc_cruise_speed;
		float speed_threshold = _mc_cruise_speed;
		const float threshold_max = target_threshold;

		if (target_threshold < 0.5f * prev_to_dest.length()) {
			/* Target threshold cannot be more than distance from previous to target */
			target_threshold = 0.5f * prev_to_dest.length();
		}

		/* Compute maximum speed at target threshold */
		if (threshold_max > _nav_rad.get()) {
			float m = (_mc_cruise_speed - _speed_at_target) / (threshold_max - _nav_rad.get());
			speed_threshold = m * (target_threshold - _nav_rad.get()) + _speed_at_target; // speed at transition
		}

		/* Either accelerate or decelerate */
		if (closest_to_dest.length() < target_threshold) {

			/* Vehicle is close to destination. Start to decelerate */

			if (!has_reached_altitude) {
				/* Altitude is not reached yet. Vehicle has to stop first before proceeding */
				_speed_at_target = 0.0f;
			}

			float acceptance_radius = _nav_rad.get();

			if (_speed_at_target < 0.01f) {
				/* If vehicle wants to stop at the target, then set acceptance radius
				 * to zero as well.
				 */
				acceptance_radius = 0.0f;
			}

			if ((target_threshold - acceptance_radius) >= SIGMA_NORM) {

				/* Slow down depending on distance to target minus acceptance radius */
				float m = (speed_threshold - _speed_at_target) / (target_threshold - acceptance_radius);
				speed_sp_track = m * (closest_to_dest.length() - acceptance_radius) + _speed_at_target; // speed at transition

			} else {
				speed_sp_track = _speed_at_target;
			}

			/* If we are close to target and the previous speed setpoint along track was smaller than
			 * current speed setpoint along track, then take over the previous one.
			 * This ensures smoothness since we anyway want to slow down.
			 */
			if ((speed_sp_prev_track < speed_sp_track)
			    && (speed_sp_track * speed_sp_prev_track > 0.0f)
			    && (speed_sp_prev_track > _speed_at_target)) {
				speed_sp_track = speed_sp_prev_track;
			}

		} else {

			/* Vehicle is still far from destination. Accelerate or keep maximum target speed. */
			float acc_track = (speed_sp_track - speed_sp_prev_track) / _deltatime;

			float yaw_diff = 0.0f;

			if (PX4_ISFINITE(_yaw_wp)) {
				yaw_diff = _wrap_pi(_yaw_wp - _yaw);
				PX4_WARN("Yaw Waypoint not finite");
			}

			/* If yaw offset is large, only accelerate with 0.5 m/s^2. */
			float acc_max = (fabsf(yaw_diff) > math::radians(_mis_yaw_error.get())) ? 0.5f : _acc_xy.get();

			if (acc_track > acc_max) {
				/* Accelerate towards target */
				speed_sp_track = acc_max * _deltatime + speed_sp_prev_track;
			}
		}

		speed_sp_track = math::constrain(speed_sp_track, 0.0f, _mc_cruise_speed);

		_pos_sp_xy = closest_pt;
		_vel_sp_xy = u_prev_to_dest * speed_sp_track;
	}
}

void FlightTaskAutoLine::_generateAltitudeSetpoints()
{
	/* Total distance between previous and target setpoint */
	const float dist = fabsf(_destination(2) - _origin(2));

	/* If target has not been reached, then compute setpoint depending on maximum velocity */
	if ((dist > SIGMA_NORM) && (fabsf(_position(2) - _destination(2)) > 0.1f)) {

		/* get various distances */
		const float dist_to_prev = fabsf(_position(2) - _origin(2));
		const float dist_to_target = fabsf(_destination(2) - _position(2));

		/* check sign */
		const bool flying_upward = _destination(2) < _position(2);

		/* Speed at threshold is by default maximum speed. Threshold defines
		 * the point in z at which vehicle slows down to reach target altitude. */
		float speed_sp = (flying_upward) ? _vel_max_up.get() : _vel_max_down.get();

		/* target threshold defines the distance to target(2) at which
		 * the vehicle starts to slow down to approach the target smoothly */
		float target_threshold = speed_sp * 1.5f;

		/* If the total distance in z is NOT 2x distance of target_threshold, we
		 * will need to adjust the final_velocity in z */
		const bool is_2_target_threshold = dist >= 2.0f * target_threshold;
		const float min_vel = 0.2f; // minimum velocity: this is needed since estimation is not perfect
		const float slope = (speed_sp - min_vel) / (target_threshold); /* defines the the acceleration when slowing down */

		if (!is_2_target_threshold) {
			/* adjust final_velocity since we are already are close
			 * to target and therefore it is not necessary to accelerate
			 * up to full speed
			 */
			target_threshold = dist * 0.5f;
			/* get the velocity at target_threshold */
			speed_sp = slope * (target_threshold) + min_vel;
		}

		/* we want to slow down */
		if (dist_to_target < target_threshold) {

			speed_sp = slope * dist_to_target + min_vel;

		} else if (dist_to_prev < target_threshold) {
			/* we want to accelerate */

			const float acc = (speed_sp - fabsf(_vel_sp_z)) / _deltatime;
			const float acc_max = (flying_upward) ? (_acc_max_up.get() * 0.5f) : (_acc_max_down.get() * 0.5f);

			if (acc > acc_max) {
				speed_sp = acc_max * _deltatime + fabsf(_vel_sp_z);
			}
		}

		/* make sure vel_sp_z is always positive */
		if (speed_sp < 0.0f) {
			PX4_WARN("speed cannot be smaller than 0");
			speed_sp = 0.0f;
		}

		/* get the sign of vel_sp_z */
		_vel_sp_z = (flying_upward) ? -speed_sp : speed_sp;
		_pos_sp_z = NAN; // We don't care about position setpoint */

	} else {

		/* Vehicle reached desired target altitude */
		_vel_sp_z = 0.0f;
		_pos_sp_z = _target(2);
	}
}
void FlightTaskAutoLine::_generateVelocitySetpoints()
{
	/* TODO: Remove velocity force logic from navigator, since
	 * navigator should only send out waypoints. */
	_setPositionSetpoint(Vector3f(NAN, NAN, _position(2)));
	Vector2f vel_sp_xy = Vector2f(&_velocity(0)).unit_or_zero() * _mc_cruise_speed;
	_setVelocitySetpoint(Vector3f(vel_sp_xy(0), vel_sp_xy(1), NAN));

	_reset();
}

float FlightTaskAutoLine::_getVelocityFromAngle(const float angle)
{
	/* Minimum cruise speed when passing waypoint */
	float min_cruise_speed = 0.0f;

	/* Make sure that cruise speed is larger than minimum*/
	if ((_mc_cruise_speed - min_cruise_speed) < SIGMA_NORM) {
		return _mc_cruise_speed;
	}

	/* Middle cruise speed is a number between maximum cruising speed and minimum cruising speed and corresponds to speed at angle of 90degrees.
	 * It needs to be always larger than minimum cruise speed. */
	float middle_cruise_speed = _cruise_speed_90.get();

	if ((middle_cruise_speed - min_cruise_speed) < SIGMA_NORM) {
		middle_cruise_speed = min_cruise_speed + SIGMA_NORM;
	}

	if ((_mc_cruise_speed - middle_cruise_speed) < SIGMA_NORM) {
		middle_cruise_speed = (_mc_cruise_speed + min_cruise_speed) * 0.5f;
	}

	/* If middle cruise speed is exactly in the middle, then compute
	 * speed linearly
	 */
	bool use_linear_approach = false;

	if (((_mc_cruise_speed + min_cruise_speed) * 0.5f) - middle_cruise_speed < SIGMA_NORM) {
		use_linear_approach = true;
	}

	/* Compute speed sp at target */
	float speed_close;

	if (use_linear_approach) {

		/* velocity close to target adjusted to angle
		 * vel_close =  m*x+q
		 */
		float slope = -(_mc_cruise_speed - min_cruise_speed) / 2.0f;
		speed_close = slope * angle + _mc_cruise_speed;

	} else {

		/* Speed close to target adjusted to angle x.
		 * speed_close = a *b ^x + c; where at angle x = 0 -> speed_close = cruise; angle x = 1 -> speed_close = middle_cruise_speed (this means that at 90degrees
		 * the velocity at target is middle_cruise_speed);
		 * angle x = 2 -> speed_close = min_cruising_speed */

		/* from maximum cruise speed, minimum cruise speed and middle cruise speed compute constants a, b and c */
		float a = -((middle_cruise_speed - _mc_cruise_speed) * (middle_cruise_speed - _mc_cruise_speed))
			  / (2.0f * middle_cruise_speed - _mc_cruise_speed - min_cruise_speed);
		float c = _mc_cruise_speed - a;
		float b = (middle_cruise_speed - c) / a;
		speed_close = a * powf(b, angle) + c;
	}

	/* speed_close needs to be in between max and min */
	return math::constrain(speed_close, min_cruise_speed, _mc_cruise_speed);
}
