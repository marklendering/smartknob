
#include "network_task.h"


NetworkTask::NetworkTask(const uint8_t task_core) : Task("Network", 2048, 1, task_core) {

}

NetworkTask::~NetworkTask() {}

void NetworkTask::run() {
    initWiFi();

    client.setServer(mqtt_server, 1883);
    while (1) {
        if (!client.connected()) {
            reconnect();
        }
        client.loop();
        delay(1);
    }
}

void NetworkTask::initWiFi() {
  Serial.print("setting wifi mode..");
  WiFi.mode(WIFI_STA);
  Serial.print("begin connecting to wifi..");
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi ..");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print('.');
    delay(1000);
  }
  Serial.println(WiFi.localIP());
}

void NetworkTask::reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (client.connect("ESP8266Client")) {
      Serial.println("connected");
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}


void NetworkTask::publishMessage() {
    client.publish("tkkrlab/flipdot", "hallo");
}

