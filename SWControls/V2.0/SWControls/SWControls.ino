#include "sc7-can-libinclude.h"
#include <Metro.h>
#include <Switch.h>
#include <serLCD.h>
#include <avr/wdt.h>
#include <SPI.h>
#include <math.h>

//#define LOOPBACK
#define DEBUG

/*Defining the bitwise functions (bitwise operators)
We're using bits to store data because there are only 8 bytes available for use in a CAN packet.
We can store all of the necessary data in a single byte and save space. This frees up space to
send other data that we might need in the future between the steering wheel and the driver controls*/
#define BIT(n)                   ( 1<<(n) ) 
#define BIT_SET(y, mask)         ( y |=  (mask) )                       //Set the value of BIT = 1
#define BIT_CLEAR(y, mask)       ( y &= ~(mask) )                       //Set the value of BIT = 0
#define BIT_FLIP(y, mask)        ( y ^=  (mask) )                       //Flip the value of BIT
#define BIT_DIFFERENT(y, x, mask)( y & mask == x & mask )               //See if two bits are different values
#define BIT_CHECK(y, bit, mask)  ( BIT_DIFFERENT(y, (bit)*mask, mask) ) //check function utilizing the previous defined different function

//Defining the bit masks
#define FWD_GEAR BIT(0)
#define REV_GEAR BIT(1)
#define HEADLIGHT BIT(2)
#define HAZARDLIGHT BIT(3)
//#define LAP_TIMER BIT(4)
#define HORN BIT(5)
#define LEFT_TURN BIT(6)
#define RIGHT_TURN BIT(7)

/*These are the bytes containing the above-mentioned bits. The two bytes will be used
to compare the switch states and determine whether there has been a change or not.*/
char young = 0xFF; //young is continuously assigned to the new switch states
char old;          //old is the previous switch states

//Conversion data for displayed measurements
#define MPS_TO_MPH 2.2369f

//Steering Wheel LCD Info - the position on the LCD
//const int SOC = 8;    //state of charge (from CAN)
const int V = 6;      //velocity (from CAN)
const int GEAR = 13;  //forward/reverse/neutral
const int LT = 8;    //lap timer
const int LIGHT = 11; //headlights/hazardlights/no lights
const int RIGHT = 15; //right turn signals
const int LEFT = 1;   //left turn signals
const int TELM = 0;   //telemetry indicator

//set up pins that connect to switch terminals
const int fgp =   6;  //forward gear
const int rgp =   7;  //reverse gear
const int hp =    9;  //headlights
const int hzp =   8;  //hazardlights
const int laptimerp =   4;  //lap timer reset
const int hornp = 5; //horn
const int ltp =   3;  //left turn
const int rtp =   A2;  //right turn

//set up metro timer
//1st: switch state reading timer - frequency at which switches are read
Metro switch_timer = Metro(100);
//2nd: CAN Transmission timer - frequency at which CAN packets are sent if switch states have not changed
Metro CAN_TX = Metro(1000);
//3rd: CAN Reception timer - duration between CAN packets received (will trigger error if it expires)
Metro CAN_RX = Metro(1000);
//4th: Notification Timer - duration for which notification is displayed
Metro notif_timer = Metro(500);
//5th: Display Timer - frequency at which display will refresh if nothing changes
Metro display_timer = Metro(500);
//6th: Turn signal blinking timer
Metro blinking_timer = Metro(500);
//7th: Debug Timer
Metro debug_timer = Metro(200);
//8th: Telemetry HB timer
Metro telmetry_timer = Metro(2500);

//CAN parameters
const byte     CAN_CS 	 = 10;
const byte	   CAN_INT	 = 2; // Interrupt #1
const uint16_t CAN_BAUD_RATE = 1000;
const byte     CAN_FREQ      = 16;
uint16_t errors;

CAN_IO CanControl(CAN_CS, CAN_INT, CAN_BAUD_RATE, CAN_FREQ); //Try initializing without interrupts for now

//Declaring switch objects (for debouncing, based on the included Switch library)
Switch laptimerreset(laptimerp);
Switch horn(hornp);

//Declaring serLCD object (display, based on the NUserLCD library)
serLCD_buffered screen(Serial1);

//setup a display structure to store the shenanigans that we neeed to display on LCD
struct LCD{
  char lapdisplay[4];    
  int lapmindisplay;     //displays the minutes in the lap
  int lapsecdisplay;     //displays the seconds in the lap
  char geardisplay;      //'F' = forward, 'R' = reverse, 'N' = neutral
  char telemetrydisplay; //'T' = telemetry on, ' ' = telemetry off
  String Lightsdisplay;  //"H" = headlights, "HZ" = hazardlights, " " = no lights
  //float SOCdisplay;        //state of charge (from CAN)
  float Veldisplay;        //velocity (from CAN)
  boolean LTdisplay;     //distinguishes left turn
  boolean RTdisplay;     //distinguishes right turn
  boolean turnsignal_on; //whether turn signal is on/off
  String notification; //notification string
};
LCD steering_wheel;

//declare functions
void setup();
void switchBitFromPin();
void switchBit();
void blnk();
void defaultDisplay();
void displayNotification();
void loop();
void initializePins();
void checkProgrammingMode();

unsigned long loopStartTime = 0;
unsigned long loopSumTime = 0;
unsigned long loopCount = 0;

unsigned long previousmillis = 0;

void setup() {
  
  // Pin Modes
  pinMode(fgp, INPUT_PULLUP);
  pinMode(rgp, INPUT_PULLUP);
  pinMode(hp, INPUT_PULLUP);
  pinMode(hzp, INPUT_PULLUP);
  pinMode(laptimerp, INPUT_PULLUP);
  pinMode(hornp, INPUT_PULLUP);
  pinMode(ltp, INPUT_PULLUP);
  pinMode(rtp, INPUT_PULLUP);

  //set Serial and screen baud rate to 9600bps
  Serial.begin(9600);
  screen.begin();
  delay(500); // Allow MCP2515 to run for 128 cycles and LCD to boot

  /*
   * PRO MICRO MUST BE PUT INTO PROGRAMMING MODE BEFORE
   * PROGRAMMING BY SETTING HAZARD/HEADLIGHT SWITCH TO
   * THE HAZARDS POSITION.
   */ checkProgrammingMode();

  // Initialize the pin states
  initializePins();
  /*
   * CAN Setup
   * Configure RB0 to take SOC and Velocity packets for the display.
   * RB1 can be used for other packets as needed.
   */
  CanControl.filters.setRB0(MASK_Sxxx,BMS_SOC_ID,0); 
  CanControl.filters.setRB1(MASK_Sxxx,MC_VELOCITY_ID,0,0,0);
  CanControl.Setup(RX0IE|RX1IE);

#ifdef LOOPBACK 
  Serial.print("Set Loopback"); 
  CanControl.controller.Mode(MODE_LOOPBACK); 
#endif

  // Enable WDT
  wdt_enable(WDTO_4S);

  //Initialize turnsignal_on state
  steering_wheel.turnsignal_on = false;
  
  steering_wheel.lapmindisplay = 0;
  steering_wheel.lapsecdisplay = 00;
  
#ifdef DEBUG
  Serial.print("CANINTE: " );
  Serial.println(CanControl.controller.Read(CANINTE), BIN);
#endif
}

//assigns appropriate value to the bit from the state of the pin
inline void switchBitFromPin(byte pin, char& out, byte mask){
  switchBit(digitalRead(pin),out, mask);
}

//helper function for the above switchBitFromPin function
inline void switchBit(bool b, char& out, byte mask) {
  if (b){
    BIT_SET(out,mask);
  }
  else{
    BIT_CLEAR(out,mask);
  }
}

//blink function used for the turn signals
inline void blnk(int a, boolean on){
  if (on)
  {
    screen.setCursor(1,a);
    if (a == LEFT){
      screen.print("<<");
    }
    else{
      screen.print(">>");
    }
    screen.setCursor(2,a);
    if (a == LEFT){
      screen.print("<<");
    }
    else{
      screen.print(">>");
    }
  }
  else
  {
    screen.setCursor(1,a);
    screen.print("  ");
    screen.setCursor(2,a);
    screen.print("  ");
  }
}

//this is the non-notification display function (what is displayed onto LCD if it's not the notification)
inline void defaultdisplay(){
  screen.clear();
  screen.setCursor(1,4);
  screen.print("LAP ");
  screen.setCursor(1,LT);
  sprintf(steering_wheel.lapdisplay,"%d:%02d",steering_wheel.lapmindisplay,steering_wheel.lapsecdisplay);
  screen.print(steering_wheel.lapdisplay);
  screen.setCursor(1,GEAR);
  screen.print(steering_wheel.geardisplay);
  screen.setCursor(2,4);
  screen.print("V:");
  screen.setCursor(2,V);
  screen.print(min(99,int(steering_wheel.Veldisplay)));
  screen.setCursor(2,LIGHT);
  screen.print(steering_wheel.Lightsdisplay);
  screen.setCursor(2,TELM);
  screen.print(steering_wheel.telemetrydisplay);
  if(steering_wheel.LTdisplay || steering_wheel.Lightsdisplay=="HZ"){
    blnk(LEFT,steering_wheel.turnsignal_on);
  }

  if(steering_wheel.RTdisplay || steering_wheel.Lightsdisplay=="HZ"){
    blnk(RIGHT,steering_wheel.turnsignal_on);
  }
}

//Notification display function
inline void displayNotification(){
  //screen.clear();
  screen.clearLine(1);
  screen.selectLine(1);
  screen.print(steering_wheel.notification);
}


void loop() {  
  #ifdef DEBUG
    loopStartTime = micros();
  #endif
  
  wdt_reset();
  old = young;
  
  /*if the metro timer runs out, then check the states of all the switches
   assign the values to the 'young' byte. Reset switch timer.*/
  if (switch_timer.check()){
    switchBitFromPin(fgp,  young,FWD_GEAR);
    switchBitFromPin(rgp,  young,REV_GEAR);
    switchBitFromPin(hp,   young,HEADLIGHT);
    switchBitFromPin(hzp,  young,HAZARDLIGHT);
    switchBitFromPin(ltp,  young,LEFT_TURN);
    switchBitFromPin(rtp,  young,RIGHT_TURN);
    
    //poll the cruisecontrol and horn and change value of bit accordingly
    laptimerreset.poll();
    if(laptimerreset.pushed()){
      previousmillis = millis();
    }
    horn.poll();
    switchBit(!horn.on(), young, HORN);
    switch_timer.reset();
  }  
  
  if (old != young || display_timer.check()){

    //possibility of switch statements?
    //Switch turnsignal_on on and off at regular intervals
    steering_wheel.turnsignal_on = !steering_wheel.turnsignal_on;

    //Display shenanigans
    /*What is generally happening is the code is checking whether the switch states have changed by juxtaposing the young and old bytes and then
    changing the members of the display structure accordingly and also changing the notification string*/

    if(!(~young & (FWD_GEAR|REV_GEAR)) && steering_wheel.geardisplay != 'N'){
      steering_wheel.geardisplay = 'N';
      steering_wheel.notification = String("Neutral Gear");
      notif_timer.reset();
    }
    if((~young & FWD_GEAR) && steering_wheel.geardisplay != 'F'){
      steering_wheel.geardisplay = 'F';
      steering_wheel.notification = String("Forward Gear");
      notif_timer.reset();
    }
    if((~young & REV_GEAR) && steering_wheel.geardisplay != 'R'){
      steering_wheel.geardisplay = 'R';
      steering_wheel.notification = String("Reverse Gear");
      notif_timer.reset();
    }

    if((~young & HEADLIGHT) && steering_wheel.Lightsdisplay != "H "){
      steering_wheel.Lightsdisplay = "H ";
      steering_wheel.notification = String("Headlights");
      notif_timer.reset();
    }
    if((~young & HAZARDLIGHT) && steering_wheel.Lightsdisplay != "HZ"){
      steering_wheel.Lightsdisplay = "HZ";
      steering_wheel.notification = String("Hazardlights");
      notif_timer.reset();
    } 
    if(!(~young & (HAZARDLIGHT|HEADLIGHT)) && steering_wheel.Lightsdisplay != "  "){
      steering_wheel.Lightsdisplay = "  ";
      steering_wheel.notification = String("All lights off");
      notif_timer.reset();
    }

    /*if((~young & CRUISE_CONTROL) && steering_wheel.CCdisplay != 'C'){
      steering_wheel.CCdisplay = 'C';
      steering_wheel.notification = String("CruiseControl on");
      notif_timer.reset();
    }  
    if((young & CRUISE_CONTROL) && steering_wheel.CCdisplay != ' '){
      steering_wheel.CCdisplay = ' ';
      steering_wheel.notification = String("CruiseControlOff");
      notif_timer.reset();
    }*/

    // Check for Telemetry disconnection
    if (telmetry_timer.check()){
      steering_wheel.telemetrydisplay = ' ';
    }

    if((~young & LEFT_TURN)){
      steering_wheel.LTdisplay = true;
    }
    else steering_wheel.LTdisplay = false;

    if((~young & RIGHT_TURN)){
      steering_wheel.RTdisplay = true;
    }
    else steering_wheel.RTdisplay = false;

    if (notif_timer.running()){
      defaultdisplay();
      displayNotification();
    }
    else{
      defaultdisplay();
    }
    steering_wheel.lapsecdisplay = (millis() - previousmillis)/1000;
    if (steering_wheel.lapsecdisplay >= 60)
    {
      steering_wheel.lapmindisplay = steering_wheel.lapsecdisplay/60;
      steering_wheel.lapsecdisplay -= steering_wheel.lapmindisplay*60;
    }
    else
    {
      steering_wheel.lapmindisplay = 0;
    }
    
    screen.update();
  }

  //If this byte is different from the one in the void setup() or the CAN_TX timer runs out, send CAN packet and reset CAN_TX timer.
  if(young != old || CAN_TX.check()){
    CanControl.Send(SW_Data(young),TXB0);
    CAN_TX.reset();
  }

  wdt_reset();

  // Fetch any potential messages from the MCP2515
  CanControl.Fetch(); 
  
  // Check whether a CAN packet has been loaded and is available to read:
  if (CanControl.Available()){
    
    // Use available CAN packets (BMS SOC and MC Velocity) to assign values to appropriate members of the data structures
    Frame& f = CanControl.Read();
 #ifdef DEBUG
    Serial.print("Received: " );
    Serial.println(f.id, HEX);
 #endif
    switch (f.id)
    {
      /*case BMS_SOC_ID:
      {
        BMS_SOC packet(f); //This is where we get the State of charge
        steering_wheel.SOCdisplay = packet.percent_SOC*100;
        #ifdef DEBUG
          Serial.print(steering_wheel.SOCdisplay);
        #endif
        CAN_RX.reset();
        break;
      }*/
      case MC_VELOCITY_ID:
      {
        MC_Velocity packet(f); // This is where we get the velocity
        steering_wheel.Veldisplay = packet.car_velocity*MPS_TO_MPH;
        #ifdef DEBUG
          Serial.print(steering_wheel.Veldisplay);
        #endif
        CAN_RX.reset();
        break;
      }
      case TELM_HEARTBEAT_ID:
      {
        steering_wheel.telemetrydisplay = 'T';
        telmetry_timer.reset();
      }
    }
  }
  // else if (CAN_RX.check()){
  // 	notif_timer().reset();
  // 	steering_wheel.notification = "Comm. lost with Driver Controls!"
  // } I guess we'll implement this later, since it hasn't been checked. 
  
  //Debug for CAN
  CanControl.FetchErrors();
  CanControl.FetchStatus();

  #ifdef DEBUG
    loopSumTime += (micros() - loopStartTime);
    loopCount += 1;
  #endif
  
  #ifdef DEBUG
    if (debug_timer.check())
    {
      Serial.print("Switches:");
      Serial.println(young,BIN);
      Serial.print("TEC/REC: ");
      Serial.print(CanControl.tec); Serial.print(", "); Serial.println(CanControl.rec);
      Serial.print("CANSTATUS: ");
      Serial.println(CanControl.canstat_register);
      Serial.print("CANINTF: ");
      Serial.println(CanControl.controller.Read(CANINTF), BIN);
      Serial.print("Average Loop Time (us): ");
      Serial.println(loopSumTime/loopCount);
      Serial.print("System time: ");
      Serial.println(millis());
      
      loopSumTime = 0;
      loopCount = 0;
    }
  #endif
}

/*
 * This function runs at startup and checks whether the headlights/hazards switch is set to hazards.
 * If it is, the board is in "Programming Mode". For some reason, the pro micro won't program corectly
 * while running in the main loop. It is necessary to put the micro into this state before programming.
 */
void checkProgrammingMode()
{    
  while (digitalRead(hzp) == LOW) 
  {
    //Do nothing if hazards is on, allowing programming to happen.
    //This delay must go before the screen printing, for some random reason.
    //Also, do not call screen.clear in here.
    screen.home();
    screen.print("Turn off Hazards to Exit PrgMd  ");
    screen.update();
    delay(500); 
  }
}

inline void initializePins()
{
  //steering_wheel.CCdisplay = ' ';
  
  if(digitalRead(fgp) == LOW){
    steering_wheel.geardisplay = 'F';
  }
  else if(digitalRead(rgp) == LOW){
    steering_wheel.geardisplay = 'R';
  }
  else {
    steering_wheel.geardisplay = 'N';
  }
  
  if(digitalRead(hp) == LOW){
    steering_wheel.Lightsdisplay = "H ";
  }
  else if(digitalRead(hzp) == LOW){
    steering_wheel.Lightsdisplay = "HZ";
  }
  else {
    steering_wheel.Lightsdisplay = "  ";
  }
}



