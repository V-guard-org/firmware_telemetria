"""The car model: the driver's pedals, wheel and key in; OBD values and IMU readings out
(docs/SIMULADOR.md §3). The simulator and the fake ESP32 both drive it.

Speed, rpm, OBD values and IMU readings all come from the same state, so they agree.
SI units inside; km/h, rpm, % and °/s only where the OBD or the IMU uses them.
The values are a first guess, taken from the #9 prototype.
"""

import math
import random

G = 9.81                        # m/s²

# Driver and car (§3.1)
MAX_ACCEL = 4.0                 # m/s² at full throttle
MAX_BRAKE = 9.0                 # m/s² at full brake
ENGINE_BRAKE = 0.3              # m/s² with the throttle released
ROLLING = 0.2                   # m/s² while moving
DRAG = 0.0012                   # m/s² per (m/s)²: grows with speed
MAX_LATERAL = 6.0               # m/s² at full steering
WHEELBASE = 2.6                 # m: slowly, the turn follows the geometry
MAX_WHEEL_TAN = 0.6             # tangent of the wheel angle at full steering
GRIP_MIN_SPEED = 0.5            # m/s: below this, only the geometry limits the turn
ECUS_MUTE_S = 0.5               # after the start button
CRANK_S = 1.0
CRANK_RPM = 250
IDLE_RPM = 800
GEAR_KMH = (0, 18, 32, 50, 70, 95)          # the 6 automatic gears, from this speed up
RPM_PER_KMH = (125, 72, 50, 38, 30, 25)
LAUNCH_KMH = 3                  # below this the torque converter slips:
LAUNCH_RPM = 1500               # the rpm rises with the throttle, up to this much over idle
THROTTLE_RPM = 300
RPM_RESPONSE = 6.0              # 1/s
ENGINE_STOP_RPM_S = 2000        # rpm lost per second once the engine stops

# OBD values (§3.2); throttle is the driver's 0 to 1
LOAD_IDLE, LOAD_PER_THROTTLE = 18, 70           # % with the engine running; 0 with it stopped
MAP_IDLE, MAP_PER_THROTTLE = 30, 65             # kPa with the engine running
MAP_STOPPED = 95                                # kPa: the air outside
THROTTLE_CLOSED, THROTTLE_PER_PEDAL = 14, 80    # % of the throttle position sensor
COOLANT_START = 72              # °C
COOLANT_HOT = 90
COOLANT_COLD = 25
WARM_UP_S = 90                  # time constants
COOL_DOWN_S = 900
IAT = 31                        # °C
VOLTAGE_RUNNING = 14.1          # V
VOLTAGE_IGNITION = 12.4

# IMU readings (§3.3), in the car's frame: x forward, y left, z up
ROLL_PER_LATERAL = 0.5          # ° of roll per m/s² of lateral acceleration
PITCH_PER_ACCEL = 0.45          # ° of pitch, nose down, per m/s² of braking
BODY_RESPONSE_S = 0.25
ACCEL_NOISE = 0.05              # m/s², standard deviation on each axis
GYRO_NOISE = 0.3                # °/s
ENGINE_VIBRATION = 0.12         # m/s² on az, at twice the engine's turning frequency

# Manobras (§3.4). Braking, turns and the lane change take over a pedal or the wheel for a while;
# then the driver's controls count again.
HARD_BRAKE_S, HARD_BRAKE = 1.6, 0.9
TURN_S, TURN_STEERING = 4.0, 0.6               # about 3.6 m/s² sideways
LANE_CHANGE_S, LANE_CHANGE_STEERING = 3.0, 0.3  # a sine, about ±1.8 m/s²
STEERING_RAMP = 0.15            # share of a turn spent turning the wheel in, and out
# The pothole and the speed bump: Gaussian pulses (height, centre in s, width in s) added to az
# and gy, first by the front axle and then by the rear one, with 70% of the force. The force
# grows with the speed: base + gain per m/s, up to base + extra.
JOLTS = {
    "pothole": {"base": 0.5, "gain": 1 / 14, "extra": 1.0,  # up to about 25 m/s² in az
                "az": ((-3.0, 0.02, 0.012), (10.5, 0.06, 0.014), (-3.0, 0.10, 0.020)),
                "gy": ((12.0, 0.05, 0.020), (-8.0, 0.10, 0.030))},
    "speed_bump": {"base": 0.6, "gain": 1 / 6, "extra": 1.2,
                   "az": ((3.5, 0.15, 0.08), (-2.2, 0.38, 0.09)),
                   "gy": ((-14.0, 0.10, 0.06), (14.0, 0.30, 0.07))},
}
REAR_AXLE_FORCE = 0.7
JOLT_S = 0.8                    # how long each axle's pulses last
SLOWEST_AXLE_SPEED = 1.5        # m/s: slower, the rear axle follows as if at this speed
HELD_S = {"hard_brake": HARD_BRAKE_S, "left_turn": TURN_S, "right_turn": TURN_S,
          "lane_change": LANE_CHANGE_S}
MANEUVERS = (*JOLTS, *HELD_S)


class CarModel:
    """One car, advanced in steps by step(dt).

    The driver sets throttle and brake (0 to 1), steering (−1 full right to +1 full left)
    and ignition, and presses start(). The model starts with the engine idling. A manobra,
    started by maneuver(), takes over a control for a while or jolts the body.
    """

    def __init__(self, rng=None, noise=True):
        self.throttle = 0.0
        self.brake = 0.0
        self.steering = 0.0
        self.noise = noise
        self._rng = rng or random.Random()
        self._ignition = True
        self.running = True
        self.speed = 0.0                    # m/s
        self.accel = 0.0                    # m/s², along x
        self.yaw_rate = 0.0                 # rad/s, positive to the left
        self.rpm = float(IDLE_RPM)
        self.gear = 1
        self.coolant = float(COOLANT_START)
        self._crank_left = 0.0
        self._mute_left = 0.0
        self._roll = 0.0                    # °
        self._pitch = 0.0
        self._vibration_phase = 0.0
        self._t = 0.0                       # s since the model started
        self._maneuvers = []                # (name, when it started)
        self._pedal = 0.0                   # the throttle the engine sees, after the manobras
        self.imu = (0.0, 0.0, G, 0.0, 0.0, 0.0)

    @property
    def ignition(self):
        return self._ignition

    @ignition.setter
    def ignition(self, on):
        self._ignition = on
        if not on:
            self.running = False
            self._crank_left = 0.0

    def start(self):
        """The start button: ECUs mute for 0.5 s, 250 rpm for 1 s, then idle. Only with the ignition on."""
        if self._ignition:
            self.running = False
            self._crank_left = CRANK_S
            self._mute_left = ECUS_MUTE_S

    def maneuver(self, name):
        """Starts a manobra (§3.4), one of MANEUVERS."""
        if name not in MANEUVERS:
            raise ValueError(f"no such maneuver: {name}")
        self._maneuvers.append((name, self._t))

    @property
    def ecus_answer(self):
        return self._ignition and self._mute_left <= 0

    @property
    def speed_kmh(self):
        return self.speed * 3.6

    def step(self, dt):
        # A jolt lasts until the rear axle is past it, at most this long.
        jolt_s = JOLT_S + WHEELBASE / SLOWEST_AXLE_SPEED
        self._maneuvers = [(name, start) for name, start in self._maneuvers
                           if self._t - start <= HELD_S.get(name, jolt_s)]
        throttle, brake, steering = self._controls()
        self._pedal = throttle
        if self._crank_left > 0:
            self._crank_left -= dt
            self.running = self._crank_left <= 0
        self._mute_left = max(0.0, self._mute_left - dt)

        # Along the road: what the engine pushes, less the brake and what holds the car back.
        accel = (MAX_ACCEL * throttle if self.running else 0.0) - MAX_BRAKE * brake
        if self.speed > 0:
            accel -= ROLLING + DRAG * self.speed ** 2
            if self.running and throttle == 0:
                accel -= ENGINE_BRAKE
        speed = max(0.0, self.speed + accel * dt)
        self.accel = (speed - self.speed) / dt
        self.speed = speed

        self._step_engine(throttle, dt)

        # The turn: 100% steering gives 6 m/s² sideways; slowly, the wheel angle limits it.
        by_geometry = self.speed * steering * MAX_WHEEL_TAN / WHEELBASE
        by_grip = steering * MAX_LATERAL / self.speed if self.speed > GRIP_MIN_SPEED else by_geometry
        self.yaw_rate = min(by_geometry, by_grip, key=abs)
        lateral = self.speed * self.yaw_rate

        # The body rolls with the lateral acceleration and dives when braking (positive pitch: nose down).
        roll = _approach(self._roll, ROLL_PER_LATERAL * lateral, dt / BODY_RESPONSE_S)
        pitch = _approach(self._pitch, -PITCH_PER_ACCEL * self.accel, dt / BODY_RESPONSE_S)
        roll_rate, pitch_rate = (roll - self._roll) / dt, (pitch - self._pitch) / dt
        self._roll, self._pitch = roll, pitch

        reading = [self.accel, lateral, G, roll_rate, pitch_rate, math.degrees(self.yaw_rate)]
        if self.noise:
            for axis, sigma in enumerate((ACCEL_NOISE,) * 3 + (GYRO_NOISE,) * 3):
                reading[axis] += self._rng.gauss(0, sigma)
            self._vibration_phase += 2 * math.pi * (2 * self.rpm / 60) * dt
            if self.running:
                reading[2] += ENGINE_VIBRATION * math.sin(self._vibration_phase)
        self._jolt(reading)
        self.imu = tuple(reading)
        self._t += dt

    def obd(self):
        """The 8 PIDs as the car's ECUs see them now, by their names in DECODIFICADOR.md §3.3."""
        throttle = self._pedal
        return {
            "load": LOAD_IDLE + LOAD_PER_THROTTLE * throttle if self.running else 0.0,
            "coolant": self.coolant,
            "map": MAP_IDLE + MAP_PER_THROTTLE * throttle if self.running else MAP_STOPPED,
            "rpm": self.rpm,
            "speed": self.speed_kmh,
            "iat": IAT,
            "throttle": THROTTLE_CLOSED + THROTTLE_PER_PEDAL * throttle,
            "voltage": VOLTAGE_RUNNING if self.running else VOLTAGE_IGNITION,
        }

    def _controls(self):
        """Throttle, brake and steering: the driver's, unless a manobra holds one of them."""
        throttle, brake = _clamp(self.throttle, 0, 1), _clamp(self.brake, 0, 1)
        steering = _clamp(self.steering, -1, 1)
        for name, start in self._maneuvers:
            if name not in HELD_S:
                continue
            u = (self._t - start) / HELD_S[name]        # 0 to 1 through the manobra
            ramp = min(1.0, u / STEERING_RAMP, (1 - u) / STEERING_RAMP)
            if name == "hard_brake":
                throttle, brake = 0.0, max(brake, HARD_BRAKE)
            elif name == "left_turn":
                steering = TURN_STEERING * ramp
            elif name == "right_turn":
                steering = -TURN_STEERING * ramp
            else:
                steering = LANE_CHANGE_STEERING * math.sin(2 * math.pi * u)
        return throttle, brake, steering

    def _jolt(self, reading):
        """Adds the potholes and speed bumps under way to az and gy."""
        rear_delay = WHEELBASE / max(self.speed, SLOWEST_AXLE_SPEED)
        for name, start in self._maneuvers:
            if name not in JOLTS:
                continue
            jolt = JOLTS[name]
            force = jolt["base"] + min(jolt["extra"], jolt["gain"] * self.speed)
            for delay, share in ((0.0, 1.0), (rear_delay, REAR_AXLE_FORCE)):
                d = self._t - start - delay
                if 0 <= d <= JOLT_S:
                    reading[2] += share * force * _pulses(jolt["az"], d)
                    reading[4] += share * force * _pulses(jolt["gy"], d)

    def _step_engine(self, throttle, dt):
        kmh = self.speed_kmh
        if self._crank_left > 0:
            self.rpm = float(CRANK_RPM)
        elif not self.running:
            self.rpm = max(0.0, self.rpm - ENGINE_STOP_RPM_S * dt)
        else:
            self.gear = sum(kmh >= lowest for lowest in GEAR_KMH)
            target = max(IDLE_RPM + (LAUNCH_RPM * throttle if kmh < LAUNCH_KMH else 0),
                         kmh * RPM_PER_KMH[self.gear - 1] + THROTTLE_RPM * throttle)
            self.rpm = _approach(self.rpm, target, RPM_RESPONSE * dt)
        target, seconds = (COOLANT_HOT, WARM_UP_S) if self.running else (COOLANT_COLD, COOL_DOWN_S)
        self.coolant = _approach(self.coolant, target, dt / seconds)


def _pulses(shape, t):
    """A sum of Gaussian pulses, each (height, centre, width), at t seconds."""
    return sum(height * math.exp(-((t - centre) / width) ** 2) for height, centre, width in shape)


def _approach(value, target, fraction):
    """One step of a first-order lag: value moves this fraction of the way to target."""
    return value + (target - value) * min(1.0, fraction)


def _clamp(value, low, high):
    return min(high, max(low, value))
