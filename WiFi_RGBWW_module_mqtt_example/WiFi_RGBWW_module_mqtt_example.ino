// for HSV to RGB
#include <RGBConverter.h>

// for MQTT subscription
#include <MQTT.h>
#include <PubSubClient.h>

// for WiFi and config
#include <ESP8266WiFi.h>
#include <WiFiManager.h>

// CIE lookup table
#include "cie1931.h"

#define MQTT_ID "Bedroom_ESP"

#define MQTT_PREFIX "/openHAB/" MQTT_ID "/"

// debug output
#if 0
#define D(...)
#else
#define D(...) Serial1.printf(__VA_ARGS__)
#endif

// use this for SDK PWM: smoother, less flickering
#define ESPRESSIF_PWM 1

#if ESPRESSIF_PWM
extern "C" {
#include <pwm.h>
}
// RGB FET
#define redPIN    1
#define greenPIN  2
#define bluePIN   0

// W FET
#define w1PIN     3
#define w2PIN     4

#define PWM_0_OUT_IO_MUX PERIPHS_IO_MUX_MTDI_U
#define PWM_0_OUT_IO_NUM 12
#define PWM_0_OUT_IO_FUNC  FUNC_GPIO12

#define PWM_1_OUT_IO_MUX PERIPHS_IO_MUX_MTDO_U
#define PWM_1_OUT_IO_NUM 15
#define PWM_1_OUT_IO_FUNC  FUNC_GPIO15

#define PWM_2_OUT_IO_MUX PERIPHS_IO_MUX_MTCK_U
#define PWM_2_OUT_IO_NUM 13
#define PWM_2_OUT_IO_FUNC  FUNC_GPIO13

#define PWM_3_OUT_IO_MUX PERIPHS_IO_MUX_MTMS_U
#define PWM_3_OUT_IO_NUM 14
#define PWM_3_OUT_IO_FUNC  FUNC_GPIO14

#define PWM_4_OUT_IO_MUX PERIPHS_IO_MUX_GPIO4_U
#define PWM_4_OUT_IO_NUM 4
#define PWM_4_OUT_IO_FUNC  FUNC_GPIO4


///#define PWM_3_OUT_IO_MUX PERIPHS_IO_MUX_U0TXD_U
///#define PWM_3_OUT_IO_NUM 1
///#define PWM_3_OUT_IO_FUNC  FUNC_GPIO1
///
///#define PWM_4_OUT_IO_MUX PERIPHS_IO_MUX_GPIO5_U
///#define PWM_4_OUT_IO_NUM 5
///#define PWM_4_OUT_IO_FUNC  FUNC_GPIO5


uint32 io_info[][3] = {
	{PWM_0_OUT_IO_MUX,PWM_0_OUT_IO_FUNC,PWM_0_OUT_IO_NUM},
	{PWM_1_OUT_IO_MUX,PWM_1_OUT_IO_FUNC,PWM_1_OUT_IO_NUM},
	{PWM_2_OUT_IO_MUX,PWM_2_OUT_IO_FUNC,PWM_2_OUT_IO_NUM},
	{PWM_3_OUT_IO_MUX,PWM_3_OUT_IO_FUNC,PWM_3_OUT_IO_NUM},
	{PWM_4_OUT_IO_MUX,PWM_4_OUT_IO_FUNC,PWM_4_OUT_IO_NUM},
};

#define PWM_CHANNELS	5  //  5:5channel ; 3:3channel

#else // ESPRESSIF_PWM

// RGB FET
#define redPIN    15
#define greenPIN  13
#define bluePIN   12


// W FET
#define w1PIN     14
#define w2PIN     4

#define PWM_CHANNELS	16

#endif // ESPRESSIF_PWM

// onbaord green LED D1
#define LEDPIN    5
// onbaord red LED D2
#define LED2PIN   1


#define LEDoff digitalWrite(LEDPIN,HIGH)
#define LEDon digitalWrite(LEDPIN,LOW)

#define LED2off digitalWrite(LED2PIN,HIGH)
#define LED2on digitalWrite(LED2PIN,LOW)

// Update these with values suitable for your network.
// TODO: replace with WifiManager option
IPAddress server(192, 168, 0, 2);

WiFiManager wifiManager;
WiFiClient wclient;
PubSubClient client(wclient, server);

int fader_speed = 0;		//< speed of the color change effect

int ledIs[PWM_CHANNELS];	//< current brightness for all channels
int ledTarget[PWM_CHANNELS];	//< target brightness for all channels

void updateLED(int pin, int delta) {
	int val = ledIs[pin];
	// do nothing if the value has been reached
	if (ledTarget[pin] == val)
		return;

	// slowly increment/decrement value towards target
	if (val+delta < ledTarget[pin])
		val += delta;
	else if (val-delta > ledTarget[pin])
		val -= delta;
	else
		val = ledTarget[pin];
	ledIs[pin] = val;

	// obtain cie1931 value for brightness
	// hack: negative values mean inverted LED (0=full brightness, 1=black)
	if (val < 0)
		val = cie_MAXVAL - cie[-val-1];
	else
		val = cie[val];

	// update PWM channel
#if ESPRESSIF_PWM
	pwm_set_duty(val, pin);
	pwm_start();
#else
	analogWrite(pin, val);
#endif
	D("V=%d PIN%d=%d\n", ledIs[pin], pin, val);
}

// set target value in the range 0..cie_RANGE (255) or -cie_RANGE..-1 for inverted LEDs
void setLEDTarget(int pin, int payload) {
	int val = constrain(payload,-cie_RANGE-1,cie_RANGE);
	ledTarget[pin] = val;
}

// set taget value in the range 0..100 (for openHAB)
void setLED100Target(int pin, String payload) {
	int val = map(payload.toInt(),0,100,0,cie_RANGE);
	setLEDTarget(pin, val);
}

RGBConverter converter;

float _h, _s, _v = 0;

// this is a hacked up copy of hsvToRgb that attempts to keep brightness when
// cycling colors. HSV goes from 33% (one channel) to 66% (two channels),
// causing noticable fluctuations. This function always keeps 66% (without
// compensating for the different subjective brightness of R vs G vs B). Still,
// this is better than HSV for a color fader.
void colorloopToRgb(double h, double s, double v, byte rgb[]) {
	double r, g, b;

	int i = int(h * 3);
	double f = h * 3 - i;
	double p = v * (1 - s);
	double q = v * (1 - f * s);
	double t = v * (1 - (1 - f) * s);

	switch(i % 3){
		case 0: r = v, g = t, b = q; break;
		case 1: r = q, g = v, b = t; break;
		case 2: r = t, g = q, b = v; break;
	}

	rgb[0] = r * 255;
	rgb[1] = g * 255;
	rgb[2] = b * 255;
}


// set target values according to HSV triple in openHAB format
void setHSV(float h, float s, float v, bool keep_brightness=false) {
	byte rgb[3];
	_h = h;
	_s = s;
	_v = v;
	if (keep_brightness)
		colorloopToRgb(h/360, s/100, v/100, rgb);
	else
		converter.hsvToRgb(h/360, s/100, v/100, rgb);

	D("H=%03d S=%03d V=%03d --> ", int(_h), int(_s), int(_v));
	D("R=%03d G=%03d B=%03d\n", rgb[0], rgb[1], rgb[2]);

	setLEDTarget(redPIN, rgb[0]);
	setLEDTarget(greenPIN, rgb[1]);
	setLEDTarget(bluePIN, rgb[2]);
}


// process a published MQTT event
void mqtt_event(const MQTT::Publish& pub) {
	Serial1.print(pub.topic());
	Serial1.print(" => ");
	Serial1.println(pub.payload_string());

	String payload = pub.payload_string();

	if(String(pub.topic()) == MQTT_PREFIX "Reset"){
		Serial1.println("Resetting CPU!\n");
		ESP.restart();
	} else
	if(String(pub.topic()) == MQTT_PREFIX "RGB"){
		fader_speed = 0;
		int c1 = payload.indexOf(';');
		int c2 = payload.indexOf(';',c1+1);

		setLED100Target(redPIN, payload);
		setLED100Target(greenPIN, payload.substring(c1+1,c2));
		setLED100Target(bluePIN, payload.substring(c2+1));
		client.publish(MQTT_PREFIX "Fader", "0");
	}
	if(String(pub.topic()) == MQTT_PREFIX "HSV"){
		fader_speed = 0;
		int c1 = payload.indexOf(',');
		int c2 = payload.indexOf(',',c1+1);

		setHSV(payload.toFloat(), payload.substring(c1+1,c2).toFloat(), payload.substring(c2+1).toFloat(), false);
		client.publish(MQTT_PREFIX "Fader", "0");

	}
	else if(String(pub.topic()) == MQTT_PREFIX "Fader"){
		fader_speed = payload.toInt();
	}
	else if(String(pub.topic()) == MQTT_PREFIX "SW1"){
		setLED100Target(w1PIN, payload);
	}
	else if(String(pub.topic()) == MQTT_PREFIX "SW2"){
		setLED100Target(w2PIN, payload);
	}
	else if(String(pub.topic()) == MQTT_PREFIX "LED1"){
		digitalWrite(LEDPIN, payload.toInt());
	}
	else if(String(pub.topic()) == MQTT_PREFIX "LED2"){
		digitalWrite(LED2PIN, payload.toInt());
	}
}

unsigned long last_tick;	//< timestamp of last loop run, for smooth fading

#if ESPRESSIF_PWM
uint32 pwm_duty_init[PWM_CHANNELS] = { 0 };
#endif

// perform (re)subscription to MQTT server
void subscribe() {
	if (client.connect(WiFi.hostname())) {
		client.subscribe(MQTT_PREFIX "+");
		Serial1.println("MQTT connected: " MQTT_ID);
		LEDoff;

	}
}

void setup() {
	// configure green and red LED pins
	pinMode(LEDPIN, OUTPUT);
	pinMode(LED2PIN, OUTPUT);

	// configure RGB LED pins
	pinMode(12, OUTPUT);
	pinMode(13, OUTPUT);
	pinMode(15, OUTPUT);

	// configure W1+W2 LED pins
	pinMode(14, OUTPUT);
	pinMode(4, OUTPUT);

	// Setup console
	Serial1.begin(115200);
	delay(10);
	Serial1.printf("\nESP RGBWW (C) Andreas H, Georg L\n\n");

	// Initialize RGBWW PWM
#if ESPRESSIF_PWM
	set_pwm_debug_en(0); //disable debug print in pwm driver
	Serial1.printf("Using SDK PWM version %d with %d channels.\n", get_pwm_version(), PWM_CHANNELS);

	pwm_init(1000, pwm_duty_init, PWM_CHANNELS, io_info);

	pwm_start();
#else
	analogWriteFreq(1000);
	analogWriteRange(cie_MAXVAL);
#endif

	// register MQTT event listener
	client.set_callback(mqtt_event);

	LEDon;

	// start WiFi registration
	wifiManager.autoConnect("H801-Config", "secret password");

	while (WiFi.status() != WL_CONNECTED) {
		LED2on;
		delay(250);
		Serial1.print(".");
		LED2off;
		delay(250);
	}

	Serial1.printf("\nWiFi connected: %s (%s)\n",
		WiFi.hostname().c_str(),
		WiFi.localIP().toString().c_str());


	subscribe();


	for (int i=0; i<PWM_CHANNELS; i++) {
		ledIs[i] = 0;
		ledTarget[i] = 0;
	}
	last_tick = micros();
}


void loop() {
	// check connectivity, perform MQTT loop
	if (client.connected())
		client.loop();
	else
		subscribe();

	unsigned long t = micros();
	if (fader_speed > 0) {
		// rotate the hue value around the clock
		_h = _h + (t-last_tick)/1000000.0*fader_speed;
		if (_h > 360)
			_h -= 360;

		setHSV(_h, _s, _v, true);
	}

	// update all PWM channels
	for (int i=0; i < PWM_CHANNELS; i++)
		updateLED(i, 1);

	last_tick = t;
	delay(5);
}
