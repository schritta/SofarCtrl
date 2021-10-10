/*****
Sofar2mqtt is a remote control interface for Sofar solar and battery inverters.
It allows remote control of the inverter and reports the invertor status, power usage, battery state etc for integration with smart home systems such as Home Assistant and Node-Red vi MQTT.  
For read only mode, it will send status messages without the inverter needing to be in passive mode.  
It's designed to run on an ESP8266 microcontroller with a TTL to RS485 module such as MAX485 or MAX3485.  
Designed to work with TTL modules with or without the DR and RE flow control pins. If your TTL module does not have these pins then just ignore the wire from D5. 

Subscribe your MQTT client to:

sofar2mqtt/state

Which provides:

running_state  
grid_voltage  Disabled
grid_current  Disabled
grid_freq  Disabled
battery_power  
battery_voltage  
battery_current  
batterySOC  
battery_temp  
battery_cycles  
grid_power  
consumption  
solarPV  
today_generation  
today_exported  
today_purchase  
today_consumption  
inverter_temp  
inverterHS_temp  
solarPVAmps  

With the inverter in Passive Mode, send MQTT messages to:

sofar2mqtt/set/standby   - send value "true"  
sofar2mqtt/set/auto   - send value "true" or "battery_save"  
sofar2mqtt/set/charge   - send values in the range 0-6000 (watts)  
sofar2mqtt/set/discharge   - send values in the range 0-6000 (watts) 

battery_save is a hybrid auto mode that will charge from excess solar but not discharge.

(c)Colin McGerty 2021 colin@mcgerty.co.uk
Thanks to Rich Platts for hybrid model code and testing.
calcCRC by angelo.compagnucci@gmail.com and jpmzometa@gmail.com
*****/
#include <Arduino.h>

//The divice name is used as the MQTT base topic. If you need more than one Sofar2mqtt on your network, give them unique names.
const char* deviceName = "Sofar2mqtt";
const char* version = "v1.0";


// Wifi parameters. Fill in your wifi network name and password.
#include <ESP8266WiFi.h>
const char* wifiName = "IMPACTMEO";
const char* wifiPassword = "0000000001";
WiFiClient wifi;

//Webserver stuff
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>

ESP8266WebServer server(80);
char jsonstring[1000];

// MQTT parameters
#include <PubSubClient.h>
const char* mqttClientID = deviceName;
const char* mqttServer = "192.168.1.94";
const uint16_t mqttPort = 1883;
const char* mqttUsername = "sonoff";
const char* mqttPassword = "tasmota";
PubSubClient mqtt(wifi);

// SoftwareSerial is used to create a second serial port, which will be deidcated to RS485.
// The built-in serial port remains available for flashing and debugging.
#include <SoftwareSerial.h>
#define SERIAL_COMMUNICATION_CONTROL_PIN D5 // Transmission set pin
#define RS485_TX HIGH
#define RS485_RX LOW
#define RXPin        D6  // Serial Receive pin
#define TXPin        D7  // Serial Transmit pin
SoftwareSerial RS485Serial(RXPin, TXPin);

// Sofar run states
#define waiting 0
#define check 1
#define normal 2
#define epState 3
#define faultState 4
#define permanentFaultState 5
#define normal1 6
unsigned int INVERTER_RUNNINGSTATE;
// Battery Save mode is a hybrid mode where the battery will charge from excess solar but not discharge.
bool BATTERYSAVE = false;
// Sofar Modbus commands.
// The two CRC bytes at the end are padded with zeros to make the frames the correct size. They get replaced using calcCRC at send time.
uint8_t slaveId = 0x01;
uint8_t readSingleRegister = 0x03;
uint8_t passiveMode = 0x42;
uint8_t getRunningState[] = {slaveId, readSingleRegister, 0x02, 0x00, 0x00, 0x04, 0x00, 0x00};
uint8_t getGridVoltage[] = {slaveId, readSingleRegister, 0x02, 0x06, 0x00, 0x01, 0x00, 0x00};
uint8_t getGridCurrent[] = {slaveId, readSingleRegister, 0x02, 0x07, 0x00, 0x02, 0x00, 0x00};
uint8_t getGridFrequency[] = {slaveId, readSingleRegister, 0x02, 0x0C, 0x00, 0x01, 0x00, 0x00};
uint8_t getBatteryPower[] = {slaveId, readSingleRegister, 0x02, 0x0D, 0x00, 0x01, 0x00, 0x00};
uint8_t getBatteryVoltage[] = {slaveId, readSingleRegister, 0x02, 0x0E, 0x00, 0x02, 0x00, 0x00};
uint8_t getBatteryCurrent[] = {slaveId, readSingleRegister, 0x02, 0x0F, 0x00, 0x10, 0x00, 0x00};
uint8_t getBatterySOC[] = {slaveId, readSingleRegister, 0x02, 0x10, 0x00, 0x02, 0x00, 0x00};
uint8_t getBatteryTemperature[] = {slaveId, readSingleRegister, 0x02, 0x11, 0x00, 0x02, 0x00, 0x00};
uint8_t getBatteryCycles[] = {slaveId, readSingleRegister, 0x02, 0x2C, 0x00, 0x01, 0x00, 0x00};
uint8_t getGridPower[] = {slaveId, readSingleRegister, 0x02, 0x12, 0x00, 0x02, 0x00, 0x00};
uint8_t getLoadPower[] = {slaveId, readSingleRegister, 0x02, 0x13, 0x00, 0x02, 0x00, 0x00};
uint8_t getSolarPV[] = {slaveId, readSingleRegister, 0x02, 0x15, 0x00, 0x02, 0x00, 0x00};
uint8_t getSolarPVToday[] = {slaveId, readSingleRegister, 0x02, 0x18, 0x00, 0x02, 0x00, 0x00};
uint8_t getSolarpv1[] = {slaveId, readSingleRegister, 0x02, 0x52, 0x00, 0x02, 0x00, 0x00};
uint8_t getSolarpv2[] = {slaveId, readSingleRegister, 0x02, 0x55, 0x00, 0x02, 0x00, 0x00};
uint8_t getLoadPowerToday[] = {slaveId, readSingleRegister, 0x02, 0x1B, 0x00, 0x02, 0x00, 0x00};
uint8_t getInternalTemp[] = {slaveId, readSingleRegister, 0x02, 0x38, 0x00, 0x02, 0x00, 0x00};
uint8_t getHeatSinkTemp[] = {slaveId, readSingleRegister, 0x02, 0x39, 0x00, 0x02, 0x00, 0x00};
uint8_t getSolarPVCurrent[] = {slaveId, readSingleRegister, 0x02, 0x36, 0x00, 0x02, 0x00, 0x00};
uint8_t setStandby[] = {slaveId, passiveMode, 0x01, 0x00, 0x55, 0x55, 0x00, 0x00};
uint8_t setAuto[] = {slaveId, passiveMode, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00};
uint8_t sendHeartbeat[] = {slaveId, 0x49, 0x22, 0x01, 0x22, 0x02, 0x00, 0x00};
uint8_t setDischarge[] = {slaveId, passiveMode, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00};
uint8_t setCharge[] = {slaveId, passiveMode, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00};

// This is the return object for the sendModbus() function. Since we are a modbus master, we
// are primarily interested in the responces to our commands.
struct modbusResponce
{
	uint8_t errorLevel;
	uint8_t data[64];
	uint8_t dataSize;
	char* errorMessage;
};

// These timers are used in the main loop.
unsigned long time_1 = 0;
#define HEARTBEAT_INTERVAL 9000
unsigned long time_2 = 0;
#define RUNSTATE_INTERVAL 5000
unsigned long time_3 = 0;
#define SEND_INTERVAL 10000
unsigned long time_4 = 0;
#define BATTERYSAVE_INTERVAL 3000

// Wemos OLED Shield set up. 64x48, pins D1 and D2 - Not necessary as we will use the webserver to get our data
//#include <SPI.h>
//#include <Wire.h>
//#include <Adafruit_GFX.h>
//#include <Adafruit_SSD1306.h>
//#define OLED_RESET 0  // GPIO0
//Adafruit_SSD1306 display(OLED_RESET);

// Update the OLED. Use "NULL" for no change or "" for an empty line. - We don't need this however I will keep the string variables active and use them on the webpage.
char* line1;
char* line2;
char* line3;
char* line4;
//void updateOLED(String line1, String line2, String line3, String line4) {
//	display.clearDisplay();
//	display.setTextSize(1);
//	display.setTextColor(WHITE);

//	display.setCursor(0,0);
//	if (line1 != "NULL") 
//	{
//		display.println(line1);
//		oledLine1 = line1;
//	}
//	else 
//	{
//		display.println(oledLine1);
//	}
//	display.setCursor(0,12);
//	if (line2 != "NULL") 
//	{
//		display.println(line2);
//		oledLine2 = line2;
//	}
//	else 
//	{
//		display.println(oledLine2);
//	}
//	display.setCursor(0,24);
//	if (line3 != "NULL") 
//	{
//		display.println(line3);
//		oledLine3 = line3;
//	}
//	else 
//	{
//		display.println(oledLine3);
//	}
//	display.setCursor(0,36);
//	if (line4 != "NULL") 
//	{
//		display.println(line4);
//		oledLine4 = line4;
//	}
//	else 
//	{
//		display.println(oledLine4);
//	}
//	display.display();
//}

// Connect to WiFi
void setup_wifi() {
	// We start by connecting to a WiFi network
	Serial.println();
	Serial.print("Connecting to ");
	Serial.println(wifiName);
//	updateOLED("NULL", "NULL", "WiFi..", "NULL"); - OLED update not used and the webserver will not work without wifi
	WiFi.mode(WIFI_STA);
	WiFi.begin(wifiName, wifiPassword);
	while (WiFi.status() != WL_CONNECTED)
	{
		delay(500);
		Serial.print(".");
//		updateOLED("NULL", "NULL", "WiFi...", "NULL"); OLED update not used and the webserver will not work without wifi
	}
	WiFi.hostname(deviceName);
	Serial.println("");
	Serial.print("WiFi connected - ESP IP address: ");
	Serial.println(WiFi.localIP());
//	updateOLED("NULL", "NULL", "WiFi....", "NULL"); OLED update not used and the webserver will not work without wifi
}
// Webserver root page
void handleRoot() {
  char temp[800];
  int sec = millis() / 1000;
  int min = sec / 60;
  int hr = min / 60;

  snprintf(temp, 800,

           "<html>\
  <head>\
    <meta http-equiv='refresh' content='5'/>\
    <title>Sofar2MQTT Web</title>\
    <style>\
      body { background-color: #cccccc; font-family: Arial, Helvetica, Sans-Serif; Color: #000088; }\
    </style>\
  </head>\
  <body>\
    <h1>Sofar2MQTT </h1>\
    <p>Uptime: %02d:%02d:%02d</p>\
  </body>\
  ",


           hr, min % 60, sec % 60
          );
  strncat(temp, line1, sizeof(temp)-1);
  strncat(temp, " <BR>", sizeof(temp)-1);
  strncat(temp, line2, sizeof(temp)-1);
  strncat(temp, " <BR>", sizeof(temp)-1);
  strncat(temp, line3, sizeof(temp)-1);
//  strncat(temp, " <BR> Battery Power:", sizeof(temp)-1);
//  strncat(temp, line4, sizeof(temp)-1);
  strncat(temp, " <BR>", sizeof(temp)-1);
  strncat(temp, "<BR><a href='/jsonOut'>JsonOutput</a><HR>", sizeof(temp)-1);
  strncat(temp, "<BR><a href='/reboot'>REBOOT DEVICE</a><HR>", sizeof(temp)-1);
  strncat(temp, "</html>", sizeof(temp)-1);
  server.send(200, "text/html", temp);
}

void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";

  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }

  server.send(404, "text/plain", message);
}

void handleJsonOut()
{

  server.send(200, "application/json", jsonstring);
}

void sendData()
{
	// Update all parameters and send to MQTT.
	if(millis() >= time_3 + SEND_INTERVAL)
	{
		time_3 +=SEND_INTERVAL;
		String state = "{";
		modbusResponce rs = sendModbus(getRunningState, sizeof(getRunningState));
		if (rs.errorLevel == 0)
		{
			unsigned int a = ((rs.data[0] << 8) | rs.data[1]);
			if (!( state == "{")) { state += ","; }
			state += "\"running_state\":"+String(a);
		}

//		modbusResponce gv = sendModbus(getGridVoltage, sizeof(getGridVoltage));
//		if (gv.errorLevel == 0)
//		{
//			unsigned int b = ((gv.data[0] << 8) | gv.data[1]);
//			if (!( state == "{")) { state += ","; }
//			state += "\"grid_voltage\":"+String(b);
//		}

//		modbusResponce gc = sendModbus(getGridCurrent, sizeof(getGridCurrent));
//		if (gc.errorLevel == 0)
//		{
//			unsigned int c = ((gc.data[0] << 8) | gc.data[1]);
//			if (!( state == "{")) { state += ","; }
//			state += "\"grid_current\":"+String(c);
//		}

//		modbusResponce gf = sendModbus(getGridFrequency, sizeof(getGridFrequency));
//		if (gf.errorLevel == 0)
//		{
//			unsigned int d = ((gf.data[0] << 8) | gf.data[1]);
//			if (!( state == "{")) { state += ","; }
//			state += "\"grid_freq\":"+String(d);
//		}
		
		modbusResponce bp = sendModbus(getBatteryPower, sizeof(getBatteryPower));
		if (bp.errorLevel == 0)
		{
			unsigned int e = ((bp.data[0] << 8) | bp.data[1]);
			if (!( state == "{")) { state += ","; }
			state += "\"battery_power\":"+String(e); 
		}
		
//		modbusResponce bv = sendModbus(getBatteryVoltage, sizeof(getBatteryVoltage));
//		if (bv.errorLevel == 0)
//		{
//			unsigned int f = ((bv.data[0] << 8) | bv.data[1]);
//			if (!( state == "{")) { state += ","; }
//			state += "\"battery_voltage\":"+String(f);
//		}
		
//		modbusResponce bc = sendModbus(getBatteryCurrent, sizeof(getBatteryCurrent));
//		if (bc.errorLevel == 0)
//		{
//			unsigned int g = ((bc.data[0] << 8) | bc.data[1]);
//			if (!( state == "{")) { state += ","; }
//			state += "\"battery_current\":"+String(g);
//		}
		
		modbusResponce bs = sendModbus(getBatterySOC, sizeof(getBatterySOC));
		if (bs.errorLevel == 0)
		{
			unsigned int h = ((bs.data[0] << 8) | bs.data[1]);
			if (!( state == "{")) { state += ","; }
       if (h < 101) {
			state += "\"batterySOC\":"+String(h); }
		}

		modbusResponce bt = sendModbus(getBatteryTemperature, sizeof(getBatteryTemperature));
		if (bt.errorLevel == 0)
		{
			unsigned int i = ((bt.data[0] << 8) | bt.data[1]);
			if (!( state == "{")) { state += ","; }
			state += "\"battery_temp\":"+String(i);
		}

		modbusResponce cy = sendModbus(getBatteryCycles, sizeof(getBatteryCycles));
		if (cy.errorLevel == 0)
		{
			unsigned int j = ((cy.data[0] << 8) | cy.data[1]);
			if (!( state == "{")) { state += ","; }
			state += "\"battery_cycles\":"+String(j);
		}

		modbusResponce gp = sendModbus(getGridPower, sizeof(getGridPower));
		if (gp.errorLevel == 0)
		{
			unsigned int k = ((gp.data[0] << 8) | gp.data[1]);
			if (!( state == "{")) { state += ","; }
			state += "\"grid_power\":"+String(k);
		}

		modbusResponce lp = sendModbus(getLoadPower, sizeof(getLoadPower));
		if (lp.errorLevel == 0)
		{
			unsigned int l = ((lp.data[0] << 8) | lp.data[1]);
			if (!( state == "{")) { state += ","; }
			state += "\"consumption\":"+String(l);
		}

		modbusResponce sp = sendModbus(getSolarPV, sizeof(getSolarPV));
		if (sp.errorLevel == 0)
		{
			unsigned int m = ((sp.data[0] << 8) | sp.data[1]);
			if (!( state == "{")) { state += ","; }
			state += "\"solarPV\":"+String(m);
		}

		modbusResponce st = sendModbus(getSolarPVToday, sizeof(getSolarPVToday));
		if (st.errorLevel == 0)
		{
			unsigned int n = ((st.data[0] << 8) | st.data[1]);
			if (!( state == "{")) { state += ","; }
			state += "\"today_generation\":"+String(n);
		}

		modbusResponce et = sendModbus(getSolarpv1, sizeof(getSolarpv1));
		if (et.errorLevel == 0)
		{
			unsigned int o = ((et.data[0] << 8) | et.data[1]);
			if (!( state == "{")) { state += ","; }
			state += "\"Solarpv1\":"+String(o);
		}

		modbusResponce it = sendModbus(getSolarpv2, sizeof(getSolarpv2));
		if (it.errorLevel == 0)
		{
			unsigned int p = ((it.data[0] << 8) | it.data[1]);
			if (!( state == "{")) { state += ","; }
			state += "\"Solarpv2\":"+String(p);
		}

		modbusResponce pt = sendModbus(getLoadPowerToday, sizeof(getLoadPowerToday));
		if (pt.errorLevel == 0)
		{
			unsigned int q = ((pt.data[0] << 8) | pt.data[1]);
			if (!( state == "{")) { state += ","; }
			state += "\"today_consumption\":"+String(q);
		}

		modbusResponce vt = sendModbus(getInternalTemp, sizeof(getInternalTemp));
		if (vt.errorLevel == 0)
		{
			unsigned int r = ((vt.data[0] << 8) | vt.data[1]);
			if (!( state == "{")) { state += ","; }
			state += "\"inverter_temp\":"+String(r);
		}

		modbusResponce ht = sendModbus(getHeatSinkTemp, sizeof(getHeatSinkTemp));
		if (ht.errorLevel == 0)
		{
			unsigned int s = ((ht.data[0] << 8) | ht.data[1]);
			if (!( state == "{")) { state += ","; }
			state += "\"inverterHS_temp\":"+String(s);
		}

		modbusResponce sc = sendModbus(getSolarPVCurrent, sizeof(getSolarPVCurrent));
		if (sc.errorLevel == 0)
		{
			unsigned int t = ((sc.data[0] << 8) | sc.data[1]);
			if (!( state == "{")) { state += ","; }
			state += "\"solarPVAmps\":"+String(t);
		}
		state = state+"}";
		
		//Prefixt the mqtt topic name with deviceName.
		String topic (deviceName);
		topic += "/state";
		sendMqtt (const_cast<char*>(topic.c_str()), state);
   state.toCharArray(jsonstring, 1000);
	}
}

// This function is executed when an MQTT message arrives on a topic that we are subscribed to.
void mqttCallback(String topic, byte* message, unsigned int length) {
	Serial.print("Message arrived on topic: ");
	Serial.print(topic);
	Serial.print(". Message: ");
	String messageTemp;

	for (int i = 0; i < length; i++) {
	Serial.print((char)message[i]);
	messageTemp += (char)message[i];
	}
	Serial.println();
	int messageValue = messageTemp.toInt();

	//Set topic names to include the deviceName.
	String standbyMode (deviceName);
	standbyMode += "/set/standby";
	String autoMode (deviceName);
	autoMode += "/set/auto";
	String chargeMode (deviceName);
	chargeMode += "/set/charge";
	String dischargeMode (deviceName);
	dischargeMode += "/set/discharge";
	
	// This is where we look at incoming messages and take action based on their content.
	if (topic==standbyMode)
	{
		BATTERYSAVE = false;
		if(messageTemp == "true")
		{
			modbusResponce responce = sendModbus(setStandby, sizeof(setStandby));
			if (responce.errorLevel == 0)
			{
				Serial.println(responce.errorMessage);
			}
		}
	}
	else if (topic==autoMode)
	{
		if(messageTemp == "true")
		{
			BATTERYSAVE = false;
			modbusResponce responce = sendModbus(setAuto, sizeof(setAuto));
			if (responce.errorLevel == 0)
			{
				Serial.println(responce.errorMessage);
			}
		}
		else if(messageTemp == "battery_save")
		{
			BATTERYSAVE = true;
		}
	}
	else if (topic==chargeMode)
	{
		if(messageTemp != "false")
		{
			BATTERYSAVE = false;
			if (messageValue > 0 && messageValue < 3001)
			{
				setCharge[4] = highByte(messageValue);
				setCharge[5] = lowByte(messageValue);
				modbusResponce responce = sendModbus(setCharge, sizeof(setCharge));
				if (responce.errorLevel == 0)
				{
					Serial.println(responce.errorMessage);
				}
			}
		}
	}
	else if (topic==dischargeMode)
	{
		if(messageTemp != "false")
		{
			BATTERYSAVE = false;
			if (messageValue > 0 && messageValue < 3001)
			{
				setDischarge[4] = highByte(messageValue);
				setDischarge[5] = lowByte(messageValue);
				modbusResponce responce = sendModbus(setDischarge, sizeof(setDischarge));
				if (responce.errorLevel == 0)
				{
					Serial.println(responce.errorMessage);
				}
			}
		}
	}
}

void batterySave()
{
	if(millis() >= time_4 + BATTERYSAVE_INTERVAL)
	{
		time_4 +=BATTERYSAVE_INTERVAL;
		if (BATTERYSAVE)
		{
			//Get grid power
			modbusResponce gp = sendModbus(getGridPower, sizeof(getGridPower));
			unsigned int p = 0;
			if (gp.errorLevel == 0)
			{
				p = ((gp.data[0] << 8) | gp.data[1]);
			}
			else
			{
				Serial.println("modbus error");
			}
			Serial.print("Grid power: ");
			Serial.println(p);
			Serial.print("Battery save mode: ");
			// Switch to auto when any power flows to the grid.
			// We leave a little wriggle room because once you start charging the battery, 
			// gridPower should be floating just above or below zero.
			if (p<65535/2 || p>65525)
			{
				//exporting to the grid
				modbusResponce responce = sendModbus(setAuto, sizeof(setAuto));
				if (responce.errorLevel == 0)
				{
					Serial.println("auto");
				}
			}
			else
			{
				//importing from the grid
				modbusResponce responce = sendModbus(setStandby, sizeof(setStandby));
				if (responce.errorLevel == 0)
				{
					Serial.println("standby");
				}
			}
		}
	}
}

// This function reconnects the ESP8266 to the MQTT broker
void mqttReconnect() 
{
	// Loop until we're reconnected
	while (!mqtt.connected()) 
	{
		Serial.print("Attempting MQTT connection...");
		line1 = "Connecting MQTT... Check your  settings";
		delay(500);
	//	updateOLED("NULL", "NULL", "NULL", "MQTT..");
		// Attempt to connect
		if (mqtt.connect(mqttClientID, mqttUsername, mqttPassword)) 
		{
			Serial.println("connected");
			delay(1000);
      line1 = "MQTT connected";
			delay(1000);
			
			//Set topic names to include the deviceName.
			String standbyMode (deviceName);
			standbyMode += "/set/standby";
			String autoMode (deviceName);
			autoMode += "/set/auto";
			String chargeMode (deviceName);
			chargeMode += "/set/charge";
			String dischargeMode (deviceName);
			dischargeMode += "/set/discharge";
	
			// Subscribe or resubscribe to topics.
			mqtt.subscribe(const_cast<char*>(standbyMode.c_str()));
			mqtt.subscribe(const_cast<char*>(autoMode.c_str()));
			mqtt.subscribe(const_cast<char*>(chargeMode.c_str()));
			mqtt.subscribe(const_cast<char*>(dischargeMode.c_str()));
	//		updateOLED("NULL", "NULL", "NULL", "");
		} 
		else 
		{
			Serial.print("failed, rc=");
			Serial.print(mqtt.state());
			Serial.println(" try again in 5 seconds");
	//		updateOLED("NULL", "NULL", "NULL", "MQTT...");
			// Wait 5 seconds before retrying
			delay(5000);
		}
	}
}

modbusResponce sendModbus(uint8_t frame[], byte frameSize)
{
	//Calculate the CRC and overwrite the last two bytes.
	unsigned int crc = calcCRC(frame, frameSize-2);
	frame[frameSize-2] = crc >> 8;
	frame[frameSize-1] = crc & 0xFF;
	//Send
	digitalWrite(SERIAL_COMMUNICATION_CONTROL_PIN, RS485_TX);
	RS485Serial.write(frame, frameSize);
	// It's important to reset the SERIAL_COMMUNICATION_CONTROL_PIN as soon as 
	// we finish sending so that the serial port can start to buffer the responce.
	digitalWrite(SERIAL_COMMUNICATION_CONTROL_PIN, RS485_RX);
	modbusResponce ret = listen();
	return ret;
}

//Listen for a responce 
modbusResponce listen()
{
	uint8_t inFrame[64];
	uint8_t inByteNum;
	uint8_t inFrameSize;
	uint8_t inFunctionCode;
	uint8_t inDataBytes;
	modbusResponce ret;
	ret.dataSize = 0;

	for (unsigned char i = 0; i < 64; i++)
	{
		inFrame[i] = 0;
	}
	inFrameSize = 0;
	inFunctionCode = 0;
	inByteNum = 0;
	inDataBytes =0;
	delay(200);
	if (RS485Serial.available())
	{
		while (RS485Serial.available()) 
		{
			byte inChar = ((byte)RS485Serial.read());
			//Process the byte
			if (inByteNum == 0 && inChar == slaveId)  //If we're looking for the first byte but it dosn't match the slave ID, we're just going to drop it.
			{
				//This might be the start of a frame. Let's presume it is for now.
				inFrame[inByteNum] = inChar;
				inByteNum++;
			}
			else if (inByteNum == 1)
			{
				//This is the second byte in a frame, where the function code lives.
				inFunctionCode = inChar;
				inFrame[inByteNum] = inChar;
				inByteNum++;
			}
			else if (inByteNum == 2)
			{
				//This is the third byte in a frame, which tells us the number of data bytes to follow.
				inDataBytes = inChar;
				inFrame[inByteNum] = inChar;
				inByteNum ++;
			}
			else if (inByteNum > 2 && inByteNum < inDataBytes + 3) //There are three bytes before the data and two after (the CRC).
			{
				//This is presumed to be a data byte.
				inFrame[inByteNum] = inChar;
				ret.data[inByteNum-3] = inChar;
				ret.dataSize++;
				inByteNum++;
			}
			else if (inByteNum == inDataBytes + 3)
			{
				//This is the first CRC byte (maybe).
				inFrame[inByteNum] = inChar;
				inByteNum++;
			}
			else if (inByteNum == inDataBytes + 4)
			{
				//This is the second CRC byte (maybe).
				inFrame[inByteNum] = inChar;
				inByteNum++;
				break;
			}
		}
		inFrameSize = inByteNum;
	}
	RS485Serial.flush();
	// Now check to see if the last two bytes are a valid CRC.
	if (checkCRC(inFrame, inFrameSize))
	{
		ret.errorLevel = 0;
		ret.errorMessage = "Valid data frame";
	}
	else
	{
		ret.errorLevel = 1;
		ret.errorMessage = "Error: invalid data frame";
	}
	return ret;
}

void sendMqtt(char* topic, String msg_str)
{
	char msg[1000];
	mqtt.setBufferSize(512);
	msg_str.toCharArray(msg, msg_str.length() + 1); //packaging up the data to publish to mqtt
	if (!(mqtt.publish(topic, msg)))
	{
		Serial.println("MQTT publish failed");
	}	
	
}

void heartbeat()
{
	//Send a heartbeat
	if(millis() >= time_1 + HEARTBEAT_INTERVAL)
	{
		time_1 +=HEARTBEAT_INTERVAL;
		
		Serial.println("Send heartbeat");
		modbusResponce responce = sendModbus(sendHeartbeat, sizeof(sendHeartbeat));
		
		// This just makes the dot on the first line of the OLED screen flash on and off with 
		// the heartbeat and clears any previous RS485 error massage that might still be there.
		if (responce.errorLevel == 0)
		{
			char* flashDot;
			if (line2 == "Online") 
				flashDot = "Online.";
			
			if (line2 == "Online.")
				flashDot = "Online";
				
			if (line3 == "RS485") 
				line3 = "";
				
			if (line4 == "ERROR")
				line4 = "";
     line2 = flashDot;
	//	updateOLED("NULL", flashDot, "NULL", "NULL");
		}
		else
		{
			Serial.println(responce.errorMessage);
	//		updateOLED("NULL", "NULL", "RS485", "ERROR");
     line2 = "RS485 ERROR";
		}
		
		//Flash the LED
		digitalWrite(LED_BUILTIN, LOW);
		delay(3);
		digitalWrite(LED_BUILTIN, HIGH);
	}
}

void updateRunstate()
{
	//Check the runstate
	if(millis() >= time_2 + RUNSTATE_INTERVAL)
	{
		time_2 +=RUNSTATE_INTERVAL;
		Serial.print("Get runstate: ");
		modbusResponce responce = sendModbus(getRunningState, sizeof(getRunningState));
		if (responce.errorLevel == 0)
		{
			INVERTER_RUNNINGSTATE = ((responce.data[0] << 8) | responce.data[1]);
			Serial.println(INVERTER_RUNNINGSTATE);
      String battwatts_str;
			switch (INVERTER_RUNNINGSTATE) {
				case waiting:
				{
					if (BATTERYSAVE)
					{
          line3 = "BATTERY SAVE MODE";
		//				updateOLED("NULL", "NULL", "Batt Save", "Waiting");
					}
					else
					{
          line3 = "STANDBY MODE";
		//				updateOLED("NULL", "NULL", "Standby", "");
					}
					break;
				}
				case check:
            line3 = "CHECKING...";
		//			updateOLED("NULL", "NULL", "Checking", "NULL");
   
					break;
				case normal:
            line3 = "NORMAL";
  //          battwatts_str = String(batteryWatts())+"W ";
  //          battwatts_str.toCharArray(line4, battwatts_str.length() + 1);
	//				updateOLED("NULL", "NULL", "normal", String(batteryWatts())+"W");
					break;
				case epState:
            line3 = "EPS MODE";
	//				updateOLED("NULL", "NULL", "EPS State", "NULL");
					break;
				case faultState:
            line3 = "FAULT - Please reset inverter";
	//				updateOLED("NULL", "NULL", "FAULT", "NULL");
					break;
				case permanentFaultState:
            line3 = "PERMANENT FAULT - Try to reset inverter";
	//				updateOLED("NULL", "NULL", "PERMFAULT", "NULL");
					break;
				default:
            line3 = "Runstate Unknown?? - Not connected"; 
	//				updateOLED("NULL", "NULL", "Runstate?", "NULL");
					break;
			}
		}
		else
		{
			Serial.println(responce.errorMessage);
      line3 = "CRC Error - Check RS485 wiring";
	//		updateOLED("NULL", "NULL", "CRC-FAULT", "NULL");
		}
	}
}

unsigned int batteryWatts()
{	
	if (INVERTER_RUNNINGSTATE == normal || INVERTER_RUNNINGSTATE == normal1)
	{
		modbusResponce responce = sendModbus(getBatteryPower, sizeof(getBatteryPower));
		if (responce.errorLevel == 0)
		{
			unsigned int w = ((responce.data[0] << 8) | responce.data[1]);
			switch (INVERTER_RUNNINGSTATE) {
				case normal:
					w = w*10;
					break;
				case normal1:
					w = (65535 - w)*10;
					break;
			}
			return w;
		}
		else
		{
			Serial.println(responce.errorMessage);
     line3 = "CRC Error - Check RS485 wiring";
//			updateOLED("NULL", "NULL", "CRC-FAULT", "NULL");
      
      return 0;
		}
	}
	else
	{
		return 0;
	}
}

void setup()
{
	Serial.begin(9600);
	pinMode(LED_BUILTIN, OUTPUT);
	
	pinMode(SERIAL_COMMUNICATION_CONTROL_PIN, OUTPUT);
	digitalWrite(SERIAL_COMMUNICATION_CONTROL_PIN, RS485_RX);
	RS485Serial.begin(9600);
	
	delay(500);

	//Turn on the OLED - We don't need this
//	display.begin(SSD1306_SWITCHCAPVCC, 0x3C);  // initialize OLED with the I2C addr 0x3C (for the 64x48)
//	display.clearDisplay();
//	display.display();
//	updateOLED(deviceName, "connecting", "", version);

	setup_wifi();
 
// setup the webserver and OTA

  ArduinoOTA.setHostname("Sofar2MQTT");
  ArduinoOTA.begin();
  server.on("/", handleRoot);
  server.onNotFound(handleNotFound);
  server.on("/jsonOut", handleJsonOut);
  server.on("/reboot", [](){
    ESP.restart();
  });
  server.begin();
  Serial.println("HTTP server started");
  line4 = "0";

	mqtt.setServer(mqttServer, mqttPort);
	mqtt.setCallback(mqttCallback);
	
	//Wake up the inverter and put it in auto mode to begin with.
	heartbeat();
	Serial.println("Set start up mode: Auto");
	sendModbus(setAuto, sizeof(setAuto));	
}

void loop()
{
  //Handle webserver and OTA requests
  ArduinoOTA.handle();
  server.handleClient();
  
	//make sure mqtt is still connected
	if (!mqtt.connected()) {
//		updateOLED("NULL", "Offline", "NULL", "NULL");
    line2 = "MQTT Offline";
		mqttReconnect();
	}
	else
	{
  line2 = "MQTT Online";
//		updateOLED("NULL", "Online", "NULL", "NULL");
	}
	
	//check mqtt for incomming messages
	if(!mqtt.loop()) {
		mqtt.connect(mqttClientID, mqttUsername, mqttPassword);
	}
	
	//Send a heartbaet to keep the inverter awake
	heartbeat();
	//Check and display the runstate
	updateRunstate();
	//Transmit all data to MQTT
	sendData();
	//Set battery save state
	batterySave();

	delay(100);
}

//calcCRC and checkCRC are based on...
//https://github.com/angeloc/simplemodbusng/blob/master/SimpleModbusMaster/SimpleModbusMaster.cpp

unsigned int calcCRC(uint8_t frame[], byte frameSize) 
{
  unsigned int temp, temp2, flag;
  temp = 0xFFFF;
  for (unsigned char i = 0; i < frameSize; i++)
  {
    temp = temp ^ frame[i];
    for (unsigned char j = 1; j <= 8; j++)
    {
      flag = temp & 0x0001;
      temp >>= 1;
      if (flag)
        temp ^= 0xA001;
    }
  }
  // Reverse byte order. 
  temp2 = temp >> 8;
  temp = (temp << 8) | temp2;
  temp &= 0xFFFF;
  return temp; // the returned value is already swopped - crcLo byte is first & crcHi byte is last
}

bool checkCRC(uint8_t frame[], byte frameSize) 
{
	unsigned int calculated_crc, recieved_crc;
	recieved_crc = ((frame[frameSize-2] << 8) | frame[frameSize-1]);
	calculated_crc = calcCRC(frame, frameSize-2);
	if (recieved_crc == calculated_crc)
	{
		return true;
	}
	else
	{
		return false;
	}
}
