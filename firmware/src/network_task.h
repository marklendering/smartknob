#ifndef NETWORK_TASK_H
#define NETWORK_TASK_H
#pragma once

#include <Arduino.h>
#include <vector>
#include <WiFi.h>
#include <PubSubClient.h>

#include "task.h"

static WiFiClient espClient;
static PubSubClient client(espClient);

class NetworkTask : public Task<NetworkTask> {
    friend class Task<NetworkTask>; // Allow base Task to invoke protected run()

    

    public:
        NetworkTask(const uint8_t task_core);
        ~NetworkTask();

        void publishMessage();

    protected:
        void run();

    private:
        const char* ssid = "www.tkkrlab.nl";
        const char* password = "hax4or2the2paxor3";
        const char* mqtt_server = "10.42.1.2";

        void initWiFi();
        void reconnect();
};

#endif
