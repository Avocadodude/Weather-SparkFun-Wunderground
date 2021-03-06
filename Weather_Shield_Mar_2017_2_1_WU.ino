

/*
 This code is based on previous wrok by Nathan Seidle and Mike Grusin at SparkFun Electronics.

 This code reads all the various sensors (wind speed, direction, rain gauge, humidty, pressure, light, batt_lvl)
 and reports it over the serial comm port to Wunderground. I am using an xbee to send it to a Digi Connectport running Xig. 
 
 Measurements are reported as specified but windspeed and rain gauge are tied to interrupts that are
 calcualted at each report.


Revision History
2.0 2017/03/04
    Changed Altitude added comment. Make sure to set the altitude for station location.
2.1 2017/03/10
    Changed windgust to send 10m reading to WU. Seems like most stations do this.
 */

#include <Wire.h> //I2C needed for sensors
#include "MPL3115A2.h" //Pressure sensor-might need to change to SparkFunMPL3115A2.h since Git repo changed. https://github.com/sparkfun/MPL3115A2_Breakout
#include "HTU21D.h" //Humidity sensor-might need to change to SparkFunHTU21D.h since Git repo changed. https://github.com/sparkfun/HTU21D_Breakout

MPL3115A2 myPressure; //Create an instance of the pressure sensor
HTU21D myHumidity; //Create an instance of the humidity sensor

//Hardware pin definitions
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
// digital I/O pins
const byte WSPEED = 3;
const byte RAIN = 2;
const byte STAT1 = 7;
const byte STAT2 = 8;

// analog I/O pins
const byte REFERENCE_3V3 = A3;
const byte LIGHT = A1;
const byte BATT = A2;
const byte WDIR = A0;
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=

//Global Variables
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
long lastSecond; //The millis counter to see when a second rolls by
byte seconds; //When it hits 60, increase the current minute
byte seconds_2m; //Keeps track of the "wind speed/dir avg" over last 2 minutes array of data
byte minutes; //Keeps track of where we are in various arrays of data
byte minutes_10m; //Keeps track of where we are in wind gust/dir over last 10 minutes array of data
long currentmillis=0;
double Tn, m;
int count = 0;
void(* resetFunc) (void) = 0;//declare reset function at address 0

long lastWindCheck = 0;
volatile long lastWindIRQ = 0;
volatile byte windClicks = 0;

//We need to keep track of the following variables:
//Wind speed/dir each update (no storage)
//Wind gust/dir over the day (no storage)
//Wind speed/dir, avg over 2 minutes (store 1 per second)
//Wind gust/dir over last 10 minutes (store 1 per minute)
//Rain over the past hour (store 1 per minute)
//Total rain over date (store one per day)

byte windspdavg[120]; //120 bytes to keep track of 2 minute average

#define WIND_DIR_AVG_SIZE 120
int winddiravg[WIND_DIR_AVG_SIZE]; //120 ints to keep track of 2 minute average
float windgust_10m[10]; //10 floats to keep track of 10 minute max
int windgustdirection_10m[10]; //10 ints to keep track of 10 minute max
volatile float rainHour[60]; //60 floating numbers to keep track of 60 minutes of rain

//These are all the weather values that wunderground expects:
int winddir = 0; // [0-360 instantaneous wind direction]
float windspeedmph = 0; // [mph instantaneous wind speed]
float windgustmph = 0; // [mph current wind gust, using software specific time period]
int windgustdir = 0; // [0-360 using software specific time period]
float windspdmph_avg2m = 0; // [mph 2 minute average wind speed mph]
int winddir_avg2m = 0; // [0-360 2 minute average wind direction]
float windgustmph_10m = 0; // [mph past 10 minutes wind gust mph ]
int windgustdir_10m = 0; // [0-360 past 10 minutes wind gust direction]
float humidity = 0; // [%]
float tempf = 0; // [temperature F]
float rainin = 0; // [rain inches over the past hour)] -- the accumulated rainfall in the past 60 min
volatile float dailyrainin = 0; // [rain inches so far today in local time]
float baromin = 30.03;// [barom in]
float pressure = 0;
float dewptf; // [dewpoint F]
float dewptc; // [dewpoint C]
float temp = 0; // [temperature C]

float batt_lvl = 11.8; //[analog value from 0 to 1023]
float light_lvl = 455; //[analog value from 0 to 1023]

// volatiles are subject to modification by IRQs
volatile unsigned long raintime, rainlast, raininterval, rain;

//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=

//Interrupt routines (these are called by the hardware interrupts, not by the main code)
//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
void rainIRQ()
// Count rain gauge bucket tips as they occur
// Activated by the magnet and reed switch in the rain gauge, attached to input D2
{
	raintime = millis(); // grab current time
	raininterval = raintime - rainlast; // calculate interval between this and last event

	if (raininterval > 10) // ignore switch-bounce glitches less than 10mS after initial edge
	{
		dailyrainin += 0.011; //Each dump is 0.011" of water
		rainHour[minutes] += 0.011; //Increase this minute's amount of rain

		rainlast = raintime; // set up for next event
	}
}

void wspeedIRQ()
// Activated by the magnet in the anemometer (2 ticks per rotation), attached to input D3
{
	if (millis() - lastWindIRQ > 10) // Ignore switch-bounce glitches less than 10ms (142MPH max reading) after the reed switch closes
	{
		lastWindIRQ = millis(); //Grab the current time
		windClicks++; //There is 1.492MPH for each click per second.
	}
}


void setup()
{
	Serial.begin(9600);
	Serial.println("Weather Shield Example");

	pinMode(STAT1, OUTPUT); //Status LED Blue
	pinMode(STAT2, OUTPUT); //Status LED Green

	pinMode(WSPEED, INPUT_PULLUP); // input from wind meters windspeed sensor
	pinMode(RAIN, INPUT_PULLUP); // input from wind meters rain gauge sensor

	pinMode(REFERENCE_3V3, INPUT);
	pinMode(LIGHT, INPUT);

	//Configure the pressure sensor
	myPressure.begin(); // Get sensor online
	myPressure.setModeBarometer(); // Measure pressure in Pascals from 20 to 110 kPa
	myPressure.setOversampleRate(7); // Set Oversample to the recommended 128
	myPressure.enableEventFlags(); // Enable all three pressure and temp event flags

	//Configure the humidity sensor
	myHumidity.begin();

	seconds = 0;
	lastSecond = millis();

	// attach external interrupt pins to IRQ functions
	attachInterrupt(0, rainIRQ, FALLING);
	attachInterrupt(1, wspeedIRQ, FALLING);

	// turn on interrupts
	interrupts();

	Serial.println("Weather Shield online!");

}

void loop()
{
	//Keep track of which minute it is
  if(millis() - lastSecond >= 1000)
	{
		digitalWrite(STAT1, HIGH); //Blink stat LED

    lastSecond += 1000;

		//Take a speed and direction reading every second for 2 minute average
		if(++seconds_2m > 119) seconds_2m = 0;

		//Calc the wind speed and direction every second for 120 second to get 2 minute average
    float currentSpeed = get_wind_speed();
    windspeedmph = currentSpeed; //update global variable for windspeed when using the printWeather() function
    //float currentSpeed = random(5); //For testing
    int currentDirection = get_wind_direction();
    windspdavg[seconds_2m] = (int)currentSpeed;
    winddiravg[seconds_2m] = currentDirection;
    //if(seconds_2m % 10 == 0) displayArrays(); //For testing

		//Check to see if this is a gust for the minute
		if(currentSpeed > windgust_10m[minutes_10m])
		{
			windgust_10m[minutes_10m] = currentSpeed;
			windgustdirection_10m[minutes_10m] = currentDirection;
		}

		//Check to see if this is a gust for the day
		if(currentSpeed > windgustmph)
		{
			windgustmph = currentSpeed;
			windgustdir = currentDirection;
		}

		if(++seconds > 59)
		{
			seconds = 0;

			if(++minutes > 59) minutes = 0;
			if(++minutes_10m > 9) minutes_10m = 0;

			rainHour[minutes] = 0; //Zero out this minute's rainfall amount
			windgust_10m[minutes_10m] = 0; //Zero out this minute's gust
		}

		 count++;
      if(count == 8)//prints roughly every x seconds for every x counts
      {
         //Report all readings every x seconds based on count
         printWeather();
         count = 0;
      }

		digitalWrite(STAT1, LOW); //Turn off stat LED
	}

  uptime();
  
}

//Calculates each of the variables that wunderground is expecting
void calcWeather()
{
	//Calc winddir
	winddir = get_wind_direction();

	//Calc windspeed
	//windspeedmph = get_wind_speed();//This is calculated in the main loop on line 179

	//Calc windgustmph
	//Calc windgustdir
	//These are calculated in the main loop

	//Calc windspdmph_avg2m
	float temp = 0;
	for(int i = 0 ; i < 120 ; i++)
		temp += windspdavg[i];
	temp /= 120.0;
	windspdmph_avg2m = temp;

	//Calc winddir_avg2m, Wind Direction
	//You can't just take the average. Google "mean of circular quantities" for more info
	//We will use the Mitsuta method because it doesn't require trig functions
	//And because it sounds cool.
	//Based on: http://abelian.org/vlf/bearings.html
	//Based on: http://stackoverflow.com/questions/1813483/averaging-angles-again
	long sum = winddiravg[0];
	int D = winddiravg[0];
	for(int i = 1 ; i < WIND_DIR_AVG_SIZE ; i++)
	{
		int delta = winddiravg[i] - D;

		if(delta < -180)
			D += delta + 360;
		else if(delta > 180)
			D += delta - 360;
		else
			D += delta;

		sum += D;
	}
	winddir_avg2m = sum / WIND_DIR_AVG_SIZE;
	if(winddir_avg2m >= 360) winddir_avg2m -= 360;
	if(winddir_avg2m < 0) winddir_avg2m += 360;

	//Calc windgustmph_10m
	//Calc windgustdir_10m
	//Find the largest windgust in the last 10 minutes
	windgustmph_10m = 0;
	windgustdir_10m = 0;
	//Step through the 10 minutes
	for(int i = 0; i < 10 ; i++)
	{
		if(windgust_10m[i] > windgustmph_10m)
		{
			windgustmph_10m = windgust_10m[i];
			windgustdir_10m = windgustdirection_10m[i];
		}
	}

	//Calc humidity
	humidity = myHumidity.readHumidity();
	//float temp_h = myHumidity.readTemperature();
	//Serial.print(" TempH:");
	//Serial.print(temp_h, 2);

	//Calc tempf from pressure sensor
	tempf = myPressure.readTempF();
	//Serial.print(" TempP:");
	//Serial.print(tempf, 2);

        //Calc temp from pressure sensor
	temp = myPressure.readTemp();
	//Serial.print(" Temp:");
	//Serial.print(temp, 2);

	//Total rainfall for the day is calculated within the interrupt
	//Calculate amount of rainfall for the last 60 minutes
	rainin = 0;
	for(int i = 0 ; i < 60 ; i++)
		rainin += rainHour[i];

        //Calc pressure
	float pressure = myPressure.readPressure();
        //Serial.print("Pressure(Pa):");
        //Serial.print(pressure, 2);
      
        float temperature = myPressure.readTempF();
        //Serial.print(" Temp(f):");
        //Serial.print(temperature, 2);
      
        //References: 
        //Definition of "altimeter setting": http://www.crh.noaa.gov/bou/awebphp/definitions_pressure.php
        //Altimeter setting: http://www.srh.noaa.gov/epz/?n=wxcalc_altimetersetting
        //Altimeter setting: http://www.srh.noaa.gov/images/epz/wxcalc/altimeterSetting.pdf
        //Verified against Boulder, CO readings: http://www.crh.noaa.gov/bou/include/webpres.php?product=webpres.txt
        
        //const int station_elevation_ft = 5374; //Must be obtained with a GPS unit
        //float station_elevation_m = station_elevation_ft * 0.3048; //I'm going to hard code this
        const int station_elevation_m = 353; //Set to elevation of station location or measurement will be reported incorrectly.
        //1 pascal = 0.01 millibars
        pressure /= 100; //pressure is now in millibars
      
        float part1 = pressure - 0.3; //Part 1 of formula
        
        const float part2 = 8.42288 / 100000.0;
        float part3 = pow((pressure - 0.3), 0.190284);
        float part4 = (float)station_elevation_m / part3;
        float part5 = (1.0 + (part2 * part4));
        float part6 = pow(part5, (1.0/0.190284));
        float altimeter_setting_pressure_mb = part1 * part6; //Output is now in adjusted millibars
        baromin = altimeter_setting_pressure_mb * 0.02953;


	//Calc dewptf
        //With relative humidity and temp, calculate a dew point
        //From: http://ag.arizona.edu/azmet/dewpoint.html
            /*function calcDewPoint(humidity, tempF)
            local tempC = (tempF - 32) * 5 / 9.0;
        
            local L = math.log(humidity / 100.0);
            local M = 17.27 * tempC;
            local N = 237.3 + tempC;
            local B = (L + (M / N)) / 17.27;
            local dewPoint = (237.3 * B) / (1.0 - B);*/
             if (temp > 0.0)
              {Tn = 243.12; m = 17.62;}
            else
              {Tn = 272.62; m = 22.46;}
          
            dewptc = (Tn*(log(humidity/100)+((m*temp)/(Tn+temp)))/(m-log(humidity/100)-((m*temp)/(Tn+temp))));

    
        //Result is in C
        //Convert back to F
        dewptf = dewptc * 9 / 5.0 + 32;

	//Calc light level
	light_lvl = get_light_level();

	//Calc battery level
	batt_lvl = get_battery_level();
        
        
}       
//Returns the voltage of the light sensor based on the 3.3V rail
//This allows us to ignore what VCC might be (an Arduino plugged into USB has VCC of 4.5 to 5.2V)
float get_light_level()
{
	float operatingVoltage = analogRead(REFERENCE_3V3);

	float lightSensor = analogRead(LIGHT);

	operatingVoltage = 3.3 / operatingVoltage; //The reference voltage is 3.3V

	lightSensor = operatingVoltage * lightSensor;

	return(lightSensor);
}

//Returns the voltage of the raw pin based on the 3.3V rail
//This allows us to ignore what VCC might be (an Arduino plugged into USB has VCC of 4.5 to 5.2V)
//Battery level is connected to the RAW pin on Arduino and is fed through two 5% resistors:
//3.9K on the high side (R1), and 1K on the low side (R2)
float get_battery_level()
{
	float voltage;
  int BatteryValue;
  BatteryValue = analogRead(A7);
  voltage = BatteryValue * (3.3 / 1024)* (10+2)/2;  //Voltage devider 

	return(voltage);
}


//Returns the instataneous wind speed
float get_wind_speed()
{
	float deltaTime = millis() - lastWindCheck; //750ms

	deltaTime /= 1000.0; //Covert to seconds

	float windSpeed = (float)windClicks / deltaTime; //3 / 0.750s = 4

	windClicks = 0; //Reset and start watching for new wind
	lastWindCheck = millis();

	windSpeed *= 1.492; //4 * 1.492 = 5.968MPH

	/* Serial.println();
	 Serial.print("Windspeed:");
	 Serial.println(windSpeed);*/

	return(windSpeed);
}

//Read the wind direction sensor, return heading in degrees
int get_wind_direction()
{
	unsigned int adc;

	adc = analogRead(WDIR); // get the current reading from the sensor

	// The following table is ADC readings for the wind direction sensor output, sorted from low to high.
	// Each threshold is the midpoint between adjacent headings. The output is degrees for that ADC reading.
	// Note that these are not in compass degree order! See Weather Meters datasheet for more information.

	if (adc < 380) return (113);
	if (adc < 393) return (68);
	if (adc < 414) return (90);
	if (adc < 456) return (158);
	if (adc < 508) return (135);
	if (adc < 551) return (203);
	if (adc < 615) return (180);
	if (adc < 680) return (23);
	if (adc < 746) return (45);
	if (adc < 801) return (248);
	if (adc < 833) return (225);
	if (adc < 878) return (338);
	if (adc < 913) return (0);
	if (adc < 940) return (293);
	if (adc < 967) return (315);
	if (adc < 990) return (270);
	return (-1); // error, disconnected?
}


//Prints the various variables directly to the port
//I don't like the way this function is written but Arduino doesn't support floats under sprintf
void printWeather()
{
	calcWeather(); //Go calc all the various sensors

	Serial.print(F("http://rtupdate.wunderground.com/weatherstation/updateweatherstation.php?ID=(stationID)&PASSWORD=(Station Password)&dateutc=now&"));
        Serial.print(F("winddir="));
	Serial.print(winddir);
        Serial.print("&");
	Serial.print(F("windspeedmph="));
	Serial.print(windspeedmph, 1);
        Serial.print("&");
	Serial.print(F("windgustmph=")); 
	Serial.print(windgustmph_10m, 1);
        Serial.print("&");
	Serial.print(F("windgustdir="));
	Serial.print(windgustdir);
        Serial.print("&");
	Serial.print(F("windspdmph_avg2m="));
	Serial.print(windspdmph_avg2m, 1);
        Serial.print("&");
	Serial.print(F("winddir_avg2m="));
	Serial.print(winddir_avg2m);
        Serial.print("&");
	Serial.print("windgustmph_10m");
	Serial.print(windgustmph_10m, 1);
        Serial.print("&");
	Serial.print(F("windgustdir_10m="));
	Serial.print(windgustdir_10m);
        Serial.print("&");
	Serial.print(F("humidity="));
	Serial.print(humidity, 1);
        Serial.print("&");
	Serial.print(F("tempf="));
	Serial.print(tempf, 1);
        Serial.print("&");
        Serial.print(F("dewptf="));
        Serial.print(dewptf,1);
        Serial.print("&");
	Serial.print(F("rainin="));
	Serial.print(rainin, 2);
        Serial.print("&");
	Serial.print(F("dailyrainin="));
	Serial.print(dailyrainin, 2);
        Serial.print("&");
	Serial.print(F("baromin="));
	Serial.print(baromin, 2);
        Serial.print("&"); 
	//Serial.print(F("batt_lvl="));
	//Serial.print(batt_lvl, 2);
        //Serial.print("&");
	Serial.print(F("light_lvl="));
	Serial.print(light_lvl, 2);
	Serial.print("&");
	Serial.println(F("action=updateraw&realtime=1&rtfreq=10"));
}

void printComma() // we do this a lot, it saves two bytes each time we call it
{
  Serial.print(F(","));
}
void uptime()
{
 long days=0;
 long hours=0;
 long mins=0;
 long secs=0;
 currentmillis=millis();
 secs = currentmillis/1000; //convect milliseconds to seconds
 mins=secs/60; //convert seconds to minutes
 hours=mins/60; //convert minutes to hours
 days=hours/24; //convert hours to days
 secs=secs-(mins*60); //subtract the coverted seconds to minutes in order to display 59 secs max 
 mins=mins-(hours*60); //subtract the coverted minutes to hours in order to display 59 minutes max
 hours=hours-(days*24); //subtract the coverted hours to days in order to display 23 hours max
 
  if(hours >= 24)
  {
   resetFunc();  //call reset 
  }
}
