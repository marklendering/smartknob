#include <SimpleFOC.h>

#include "motor_task.h"
#if SENSOR_MT6701
#include "mt6701_sensor.h"
#endif
#if SENSOR_TLV
#include "tlv_sensor.h"
#endif
#include "util.h"


// #### 
// Hardware-specific motor calibration constants.
// Run calibration once at startup, then update these constants with the calibration results.
static const float ZERO_ELECTRICAL_OFFSET = 4.16;
static const Direction FOC_DIRECTION = Direction::CCW;
static const int MOTOR_POLE_PAIRS = 7;
// ####


static const float DEAD_ZONE_DETENT_PERCENT = 0.2;
static const float DEAD_ZONE_RAD = 1 * _PI / 180;

static const float IDLE_VELOCITY_EWMA_ALPHA = 0.001;
static const float IDLE_VELOCITY_RAD_PER_SEC = 0.05;
static const uint32_t IDLE_CORRECTION_DELAY_MILLIS = 500;
static const float IDLE_CORRECTION_MAX_ANGLE_RAD = 5 * PI / 180;
static const float IDLE_CORRECTION_RATE_ALPHA = 0.0005;


MotorTask::MotorTask(const uint8_t task_core) : Task("Motor", 2048, 1, task_core) {
    queue_ = xQueueCreate(5, sizeof(Command));
    assert(queue_ != NULL);
}

MotorTask::~MotorTask() {}


#if SENSOR_TLV
    TlvSensor encoder = TlvSensor();
#elif SENSOR_MT6701
    MT6701Sensor encoder = MT6701Sensor();
#endif

Commander command = Commander(Serial);


void MotorTask::run() {

    driver.voltage_power_supply = 5;
    driver.init();

    #if SENSOR_TLV
    encoder.init(Wire, false);
    #endif

    #if SENSOR_MT6701
    encoder.init();
    #endif

    motor.linkDriver(&driver);

    motor.controller = MotionControlType::torque;
    motor.voltage_limit = 5;
    motor.velocity_limit = 10000;
    motor.linkSensor(&encoder);

    // Not actually using the velocity loop built into SimpleFOC; but I'm using those PID variables
    // to run PID for torque (and SimpleFOC studio supports updating them easily over serial for tuning)
    motor.PID_velocity.P = 4;
    motor.PID_velocity.I = 0;
    motor.PID_velocity.D = 0.04;
    motor.PID_velocity.output_ramp = 10000;
    motor.PID_velocity.limit = 10;

    motor.init();

    encoder.update();
    delay(10);

    motor.pole_pairs = MOTOR_POLE_PAIRS;
    motor.initFOC(ZERO_ELECTRICAL_OFFSET, FOC_DIRECTION);

    bool calibrate = false;

    Serial.println("Press Y to run calibration");
    uint32_t t = millis();
    while (millis() - t < 3000) {
        if (Serial.read() == 'Y') {
            calibrate = true;
            break;
        }
        delay(10);
    }
    if (calibrate) {
        this->calibrate();
    }

    Serial.println(motor.zero_electric_angle);

    motor.monitor_downsample = 0; // disable monitor at first - optional

    // disableCore0WDT();

    float current_detent_center = motor.shaft_angle;
    KnobConfig config = {
        .num_positions = 2,
        .position = 0,
        .position_width_radians = 60 * _PI / 180,
        .detent_strength_unit = 0,
    };

    float idle_check_velocity_ewma = 0;
    uint32_t last_idle_start = 0;
    uint32_t last_publish = 0;

    while (1) {
        motor.loopFOC();

        // Check queue for pending requests from other tasks
        Command command;
        if (xQueueReceive(queue_, &command, 0) == pdTRUE) {
            switch (command.command_type) {
                case CommandType::CONFIG: {
                    // Change haptic input mode
                    config = command.data.config;
                    Serial.println("Got new config");
                    current_detent_center = motor.shaft_angle;
                    #if SK_INVERT_ROTATION
                        current_detent_center = -motor.shaft_angle;
                    #endif

                    // Update derivative factor of torque controller based on detent width.
                    // If the D factor is large on coarse detents, the motor ends up making noise because the P&D factors amplify the noise from the sensor.
                    // This is a piecewise linear function so that fine detents (small width) get a higher D factor and coarse detents get a small D factor.
                    // Fine detents need a nonzero D factor to artificially create "clicks" each time a new value is reached (the P factor is small
                    // for fine detents due to the smaller angular errors, and the existing P factor doesn't work well for very small angle changes (easy to
                    // get runaway due to sensor noise & lag)).
                    // TODO: consider eliminating this D factor entirely and just "play" a hardcoded haptic "click" (e.g. a quick burst of torque in each
                    // direction) whenever the position changes when the detent width is too small for the P factor to work well.
                    const float derivative_lower_strength = config.detent_strength_unit * 0.08;
                    const float derivative_upper_strength = config.detent_strength_unit * 0.02;
                    const float derivative_position_width_lower = radians(3);
                    const float derivative_position_width_upper = radians(8);
                    const float raw = derivative_lower_strength + (derivative_upper_strength - derivative_lower_strength)/(derivative_position_width_upper - derivative_position_width_lower)*(config.position_width_radians - derivative_position_width_lower);
                    motor.PID_velocity.D = CLAMP(
                        raw,
                        min(derivative_lower_strength, derivative_upper_strength),
                        max(derivative_lower_strength, derivative_upper_strength)
                    );
                    break;
                }
                case CommandType::HAPTIC: {
                    // Play a hardcoded haptic "click"
                    float strength = command.data.haptic.press ? 5 : 1.5;
                    motor.move(strength);
                    for (uint8_t i = 0; i < 3; i++) {
                        motor.loopFOC();
                        delay(1);
                    }
                    motor.move(-strength);
                    for (uint8_t i = 0; i < 3; i++) {
                        motor.loopFOC();
                        delay(1);
                    }
                    motor.move(0);
                    motor.loopFOC();
                    break;
                }
            }
        }

        // If we are not moving and we're close to the center (but not exactly there), slowly adjust the centerpoint to match the current position
        idle_check_velocity_ewma = motor.shaft_velocity * IDLE_VELOCITY_EWMA_ALPHA + idle_check_velocity_ewma * (1 - IDLE_VELOCITY_EWMA_ALPHA);
        if (fabsf(idle_check_velocity_ewma) > IDLE_VELOCITY_RAD_PER_SEC) {
            last_idle_start = 0;
        } else {
            if (last_idle_start == 0) {
                last_idle_start = millis();
            }
        }
        if (last_idle_start > 0 && millis() - last_idle_start > IDLE_CORRECTION_DELAY_MILLIS && fabsf(motor.shaft_angle - current_detent_center) < IDLE_CORRECTION_MAX_ANGLE_RAD) {
            current_detent_center = motor.shaft_angle * IDLE_CORRECTION_RATE_ALPHA + current_detent_center * (1 - IDLE_CORRECTION_RATE_ALPHA);
        }

        // Check where we are relative to the current nearest detent; update our position if we've moved far enough to snap to another detent
        float angle_to_detent_center = motor.shaft_angle - current_detent_center;
        #if SK_INVERT_ROTATION
            angle_to_detent_center = -motor.shaft_angle - current_detent_center;
        #endif
        if (angle_to_detent_center > config.position_width_radians * config.snap_point && (config.num_positions <= 0 || config.position > 0)) {
            current_detent_center += config.position_width_radians;
            angle_to_detent_center -= config.position_width_radians;
            config.position--;
        } else if (angle_to_detent_center < -config.position_width_radians * config.snap_point && (config.num_positions <= 0 || config.position < config.num_positions - 1)) {
            current_detent_center -= config.position_width_radians;
            angle_to_detent_center += config.position_width_radians;
            config.position++;
        }

        float dead_zone_adjustment = CLAMP(
            angle_to_detent_center,
            fmaxf(-config.position_width_radians*DEAD_ZONE_DETENT_PERCENT, -DEAD_ZONE_RAD),
            fminf(config.position_width_radians*DEAD_ZONE_DETENT_PERCENT, DEAD_ZONE_RAD));

        bool out_of_bounds = config.num_positions > 0 && ((angle_to_detent_center > 0 && config.position == 0) || (angle_to_detent_center < 0 && config.position == config.num_positions - 1));
        motor.PID_velocity.limit = 10; //out_of_bounds ? 10 : 3;
        motor.PID_velocity.P = out_of_bounds ? config.endstop_strength_unit * 4 : config.detent_strength_unit * 4;


        // Apply motor torque based on our angle to the nearest detent (detent strength, etc is handled by the PID_velocity parameters)
        if (fabsf(motor.shaft_velocity) > 60) {
            // Don't apply torque if velocity is too high (helps avoid positive feedback loop/runaway)
            motor.move(0);
        } else {
            float torque = motor.PID_velocity(-angle_to_detent_center + dead_zone_adjustment);
            #if SK_INVERT_ROTATION
                torque = -torque;
            #endif
            motor.move(torque);
        }

        // Publish current status to other registered tasks periodically
        if (millis() - last_publish > 10) {
            publish({
                .current_position = config.position,
                .sub_position_unit = -angle_to_detent_center / config.position_width_radians,
                .config = config,
            });
            last_publish = millis();
        }

        motor.monitor();

        delay(1);
    }
}

void MotorTask::setConfig(const KnobConfig& config) {
    Command command = {
        .command_type = CommandType::CONFIG,
        .data = {
            .config = config,
        }
    };
    xQueueSend(queue_, &command, portMAX_DELAY);
}


void MotorTask::playHaptic(bool press) {
    Command command = {
        .command_type = CommandType::HAPTIC,
        .data = {
            .haptic = {
                .press = press,
            },
        }
    };
    xQueueSend(queue_, &command, portMAX_DELAY);
}


void MotorTask::addListener(QueueHandle_t queue) {
    listeners_.push_back(queue);
}

void MotorTask::publish(const KnobState& state) {
    for (auto listener : listeners_) {
        xQueueOverwrite(listener, &state);
    }
}

void MotorTask::calibrate() {
    // SimpleFOC is supposed to be able to determine this automatically (if you omit params to initFOC), but
    // it seems to have a bug (or I've misconfigured it) that gets both the offset and direction very wrong!
    // So this value is based on experimentation.
    // TODO: dig into SimpleFOC calibration and find/fix the issue

    Serial.println("\n\n\nStarting calibration, please do not touch to motor until complete!");

    motor.controller = MotionControlType::angle_openloop;
    motor.pole_pairs = 1;
    motor.initFOC(0, Direction::CW);

    float a = 0;

    // #### Determine direction motor rotates relative to angle sensor
    for (uint8_t i = 0; i < 200; i++) {
        encoder.update();
        motor.move(a);
        delay(1);
    }
    float start_sensor = encoder.getAngle();

    for (; a < 3 * _2PI; a += 0.01) {
        encoder.update();
        motor.move(a);
        delay(1);
    }

    for (uint8_t i = 0; i < 200; i++) {
        encoder.update();
        delay(1);
    }
    float end_sensor = encoder.getAngle();


    motor.voltage_limit = 0;
    motor.move(a);

    Serial.println();

    // TODO: check for no motor movement!

    Serial.print("Sensor measures positive for positive motor rotation: ");
    if (end_sensor > start_sensor) {
        Serial.println("YES, Direction=CW");
        motor.initFOC(0, Direction::CW);
    } else {
        Serial.println("NO, Direction=CCW");
        motor.initFOC(0, Direction::CCW);
    }


    // #### Determine pole-pairs
    // Rotate 20 electrical revolutions and measure mechanical angle traveled, to calculate pole-pairs
    uint8_t electrical_revolutions = 20;
    Serial.printf("Going to measure %d electrical revolutions...\n", electrical_revolutions);
    motor.voltage_limit = 5;
    motor.move(a);
    Serial.println("Going to electrical zero...");
    float destination = a + _2PI;
    for (; a < destination; a += 0.03) {
        encoder.update();
        motor.move(a);
        delay(1);
    }
    Serial.println("pause..."); // Let momentum settle...
    for (uint16_t i = 0; i < 1000; i++) {
        encoder.update();
        delay(1);
    }
    Serial.println("Measuring...");

    start_sensor = motor.sensor_direction * encoder.getAngle();
    destination = a + electrical_revolutions * _2PI;
    for (; a < destination; a += 0.03) {
        encoder.update();
        motor.move(a);
        delay(1);
    }
    for (uint16_t i = 0; i < 1000; i++) {
        encoder.update();
        motor.move(a);
        delay(1);
    }
    end_sensor = motor.sensor_direction * encoder.getAngle();
    motor.voltage_limit = 0;
    motor.move(a);

    if (fabsf(motor.shaft_angle - motor.target) > 1 * PI / 180) {
        Serial.println("ERROR: motor did not reach target!");
        while(1) {}
    }

    float electrical_per_mechanical = electrical_revolutions * _2PI / (end_sensor - start_sensor);
    Serial.print("Electrical angle / mechanical angle (i.e. pole pairs) = ");
    Serial.println(electrical_per_mechanical);

    int measured_pole_pairs = (int)round(electrical_per_mechanical);
    Serial.printf("Pole pairs set to %d\n", measured_pole_pairs);

    delay(1000);


    // #### Determine mechanical offset to electrical zero
    // Measure mechanical angle at every electrical zero for several revolutions
    motor.voltage_limit = 5;
    motor.move(a);
    float offset_x = 0;
    float offset_y = 0;
    float destination1 = (floor(a / _2PI) + measured_pole_pairs / 2.) * _2PI;
    float destination2 = (floor(a / _2PI)) * _2PI;
    for (; a < destination1; a += 0.4) {
        motor.move(a);
        delay(100);
        for (uint8_t i = 0; i < 100; i++) {
            encoder.update();
            delay(1);
        }
        float real_electrical_angle = _normalizeAngle(a);
        float measured_electrical_angle = _normalizeAngle( (float)(motor.sensor_direction * measured_pole_pairs) * encoder.getMechanicalAngle()  - 0);

        float offset_angle = measured_electrical_angle - real_electrical_angle;
        offset_x += cosf(offset_angle);
        offset_y += sinf(offset_angle);

        Serial.print(degrees(real_electrical_angle));
        Serial.print(", ");
        Serial.print(degrees(measured_electrical_angle));
        Serial.print(", ");
        Serial.println(degrees(_normalizeAngle(offset_angle)));
    }
    for (; a > destination2; a -= 0.4) {
        motor.move(a);
        delay(100);
        for (uint8_t i = 0; i < 100; i++) {
            encoder.update();
            delay(1);
        }
        float real_electrical_angle = _normalizeAngle(a);
        float measured_electrical_angle = _normalizeAngle( (float)(motor.sensor_direction * measured_pole_pairs) * encoder.getMechanicalAngle()  - 0);

        float offset_angle = measured_electrical_angle - real_electrical_angle;
        offset_x += cosf(offset_angle);
        offset_y += sinf(offset_angle);

        Serial.print(degrees(real_electrical_angle));
        Serial.print(", ");
        Serial.print(degrees(measured_electrical_angle));
        Serial.print(", ");
        Serial.println(degrees(_normalizeAngle(offset_angle)));
    }
    motor.voltage_limit = 0;
    motor.move(a);

    float avg_offset_angle = atan2f(offset_y, offset_x);


    // #### Apply settings
    // TODO: save to non-volatile storage
    motor.pole_pairs = measured_pole_pairs;
    motor.zero_electric_angle = avg_offset_angle + _3PI_2;
    motor.voltage_limit = 5;
    motor.controller = MotionControlType::torque;

    Serial.print("\n\nRESULTS:\n  Update these constants at the top of " __FILE__ "\n  ZERO_ELECTRICAL_OFFSET: ");
    Serial.println(motor.zero_electric_angle);
    Serial.print("  FOC_DIRECTION: ");
    if (motor.sensor_direction == Direction::CW) {
        Serial.println("Direction::CW");
    } else {
        Serial.println("Direction::CCW");
    }
    Serial.printf("  MOTOR_POLE_PAIRS: %d\n", motor.pole_pairs);
    delay(2000);
}
