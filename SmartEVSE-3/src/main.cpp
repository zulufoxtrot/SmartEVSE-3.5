#include <ArduinoJson.h>
#include <SPI.h>
#include <Preferences.h>

#include <FS.h>

#include <WiFi.h>
#include "network.h"
#include "esp_ota_ops.h"
#include "mbedtls/md_internal.h"

#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <Update.h>

#include <Logging.h>
#include <ModbusServerRTU.h>        // Slave/node
#include <ModbusClientRTU.h>        // Master
#include <time.h>

#include <soc/sens_reg.h>
#include <soc/sens_struct.h>
#include <driver/adc.h>
#include <esp_adc_cal.h>

#include <soc/rtc_io_struct.h>

#include "main.h"
#include "glcd.h"
#include "utils.h"
#include "OneWire.h"
#include "modbus.h"
#include "meter.h"

//OCPP includes
#if ENABLE_OCPP
#include <MicroOcpp.h>
#include <MicroOcppMongooseClient.h>
#include <MicroOcpp/Core/Configuration.h>
#include <MicroOcpp/Core/Context.h>
#endif //ENABLE_OCPP

#if SMARTEVSE_VERSION == 4
#include <esp_sleep.h>
#include <driver/uart.h>

#include "wchisp.h"
#include "Melopero_RV3028.h"
#include "qca.h"

#define CIRCUIT_METER 0                                                         // Electric meter used for EVSE Circuit
#define CIRCUIT_METER_ADDRESS 13
#define INITIALIZED 0
uint8_t Initialized = INITIALIZED;                                              // When first powered on, the settings need to be initialized.
uint8_t CircuitMeter = CIRCUIT_METER;
uint8_t CircuitMeterAddress = CIRCUIT_METER_ADDRESS;

SPIClass QCA_SPI1(FSPI);  // The ESP32-S3 has two usable SPI busses FSPI and HSPI
SPIClass LCD_SPI2(HSPI);

Melopero_RV3028 rtc;

/*    Commands send from ESP32 to CH32V203 over Uart
/    cmd        Name           Answer/data        Comments
/---------------------------------------------------------------------------------------------------------------------------------
/    Ver?    Version           0001              Version of CH32 software
/    Stat?   Status                              State, Amperage, PP pin, SSR outputs, ACT outputs, VCC enable, Lock input, RCM, Temperature, Error
/    Amp:    Set AMP           160               Set Chargecurrent A (*10)
/    Con:    Set Contactors    0-3               0= Both Off, 1= SSR1 ON, 2= SSR2 ON, 3= Both ON
/    Vcc:    Set VCC           0-1               0= VCC Off, 1= VCC ON
/    Sol:    Set Solenoid      0-3               0= Both Off, 1= LOCK_R ON, 2= LOCK_W ON, 3= Both ON (or only lock/unlock?)
/    Led:    Set Led color                       RGB, Fade speed, Blink
/    485:    Modbus data
/
/    Bij wegvallen ZC -> Solenoid unlock (indien locked)

*/

struct rtcTime rtcTS;

uint8_t RTCBackupSource = BATTERY;
uint8_t PwrPanic = 1;           // enabled
uint8_t CommState = COMM_OFF;
uint8_t ModemPwr = 1;           // Enable the power to the Modem

uint8_t MainVersion = 0;        // Mainboard software version


// Power Panic handler
// Shut down ESP to conserve the power we have left. RTC will automatically store powerdown timestamp
// We can store some important data in flash storage or the RTC chip (2 bytes)
//
void PowerPanicESP() {

    _LOG_D("Power Panic!\n");
    ledcWrite(LCD_CHANNEL, 0);                 // LCD Backlight off

    // Stop SPI bus, and set all QCA data lines low
    // TODO: store important information.

    gpio_wakeup_enable(GPIO_NUM_8, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    esp_light_sleep_start();
    // ESP32 is now in light sleep mode

    // It will re-enable everything as soon it has woken up again.
    // When using USB, you will have to unplug, and replug to re-establish the connection

    _LOG_D("Power Back up!\n");

    ledcWrite(LCD_CHANNEL, 50);                 // LCD Backlight on
}


#endif //SMARTEVSE_VERSION

#if SMARTEVSE_VERSION == 3
// Create a ModbusRTU server, client and bridge instance on Serial1
ModbusServerRTU MBserver(2000, PIN_RS485_DIR);     // TCP timeout set to 2000 ms
ModbusClientRTU MBclient(PIN_RS485_DIR);
#endif //SMARTEVSE_VERSION

hw_timer_t * timerA = NULL;
Preferences preferences;

static esp_adc_cal_characteristics_t * adc_chars_CP;
static esp_adc_cal_characteristics_t * adc_chars_PP;
static esp_adc_cal_characteristics_t * adc_chars_Temperature;

struct ModBus MB;          // Used by SmartEVSE fuctions

extern unsigned char RFID[8];

const char StrStateName[15][13] = {"A", "B", "C", "D", "COMM_B", "COMM_B_OK", "COMM_C", "COMM_C_OK", "Activate", "B1", "C1", "MODEM1", "MODEM2", "MODEM_OK", "MODEM_DENIED"};
const char StrStateNameWeb[15][17] = {"Ready to Charge", "Connected to EV", "Charging", "D", "Request State B", "State B OK", "Request State C", "State C OK", "Activate", "Charging Stopped", "Stop Charging", "Modem Setup", "Modem Request", "Modem Done", "Modem Denied"};
const char StrErrorNameWeb[9][20] = {"None", "No Power Available", "Communication Error", "Temperature High", "EV Meter Comm Error", "RCM Tripped", "Waiting for Solar", "Test IO", "Flash Error"};
const char StrMode[3][8] = {"Normal", "Smart", "Solar"};
const char StrAccessBit[2][6] = {"Deny", "Allow"};
const char StrRFIDStatusWeb[8][20] = {"Ready to read card","Present", "Card Stored", "Card Deleted", "Card already stored", "Card not in storage", "Card Storage full", "Invalid" };

// Global data


// The following data will be updated by eeprom/storage data at powerup:
uint16_t MaxMains = MAX_MAINS;                                              // Max Mains Amps (hard limit, limited by the MAINS connection) (A)
uint16_t MaxSumMains = MAX_SUMMAINS;                                        // Max Mains Amps summed over all 3 phases, limit used by EU capacity rate
                                                                            // see https://github.com/serkri/SmartEVSE-3/issues/215
                                                                            // 0 means disabled, allowed value 10 - 600 A
uint8_t MaxSumMainsTime = MAX_SUMMAINSTIME;                                 // Number of Minutes we wait when MaxSumMains is exceeded, before we stop charging
uint16_t MaxSumMainsTimer = 0;
uint16_t GridRelayMaxSumMains = GRID_RELAY_MAX_SUMMAINS;                    // Max Mains Amps summed over all 3 phases, switched by relay provided by energy provider
                                                                            // Meant to obey par 14a of Energy Industry Act, where the provider can switch a device
                                                                            // down to 4.2kW by a relay connected to the "switch" connectors.
                                                                            // you will have to set the "Switch" setting to "GridRelay",
                                                                            // and connect the relay to the switch terminals
                                                                            // When the relay opens its contacts, power will be reduced to 4.2kW
                                                                            // The relay is only allowed on the Master
bool GridRelayOpen = false;                                                 // The read status of the relay
bool CustomButton = false;                                                  // The status of the custom button
uint16_t MaxCurrent = MAX_CURRENT;                                          // Max Charge current (A)
uint16_t MinCurrent = MIN_CURRENT;                                          // Minimal current the EV is happy with (A)
uint8_t Mode = MODE;                                                        // EVSE mode (0:Normal / 1:Smart / 2:Solar)
uint32_t CurrentPWM = 0;                                                    // Current PWM duty cycle value (0 - 1024)
int8_t InitialSoC = -1;                                                     // State of charge of car
int8_t FullSoC = -1;                                                        // SoC car considers itself fully charged
int8_t ComputedSoC = -1;                                                    // Estimated SoC, based on charged kWh
int8_t RemainingSoC = -1;                                                   // Remaining SoC, based on ComputedSoC
int32_t TimeUntilFull = -1;                                                 // Remaining time until car reaches FullSoC, in seconds
int32_t EnergyCapacity = -1;                                                // Car's total battery capacity
int32_t EnergyRequest = -1;                                                 // Requested amount of energy by car
char EVCCID[32];                                                            // Car's EVCCID (EV Communication Controller Identifer)
char RequiredEVCCID[32];                                                    // Required EVCCID before allowing charging

bool CPDutyOverride = false;
uint8_t Lock = LOCK;                                                        // Cable lock (0:Disable / 1:Solenoid / 2:Motor)
uint16_t MaxCircuit = MAX_CIRCUIT;                                          // Max current of the EVSE circuit (A)
uint8_t Config = CONFIG;                                                    // Configuration (0:Socket / 1:Fixed Cable)
uint8_t LoadBl = LOADBL;                                                    // Load Balance Setting (0:Disable / 1:Master / 2-8:Node)
uint8_t Switch = SWITCH;                                                    // External Switch (0:Disable / 1:Access B / 2:Access S / 
                                                                            // 3:Smart-Solar B / 4:Smart-Solar S / 5: Grid Relay
                                                                            // 6:Custom B / 7:Custom S)
                                                                            // B=momentary push <B>utton, S=toggle <S>witch
uint8_t RCmon = RC_MON;                                                     // Residual Current Monitor (0:Disable / 1:Enable)
uint8_t AutoUpdate = AUTOUPDATE;                                            // Automatic Firmware Update (0:Disable / 1:Enable)
uint16_t StartCurrent = START_CURRENT;
uint16_t StopTime = STOP_TIME;
uint16_t ImportCurrent = IMPORT_CURRENT;
struct DelayedTimeStruct DelayedStartTime;
struct DelayedTimeStruct DelayedStopTime;
uint8_t DelayedRepeat;                                                      // 0 = no repeat, 1 = daily repeat
uint8_t LCDlock = LCD_LOCK;                                                 // 0 = LCD buttons operational, 1 = LCD buttons disabled
uint8_t Grid = GRID;                                                        // Type of Grid connected to Sensorbox (0:4Wire / 1:3Wire )
uint8_t SB2_WIFImode = SB2_WIFI_MODE;                                       // Sensorbox-2 WiFi Mode (0:Disabled / 1:Enabled / 2:Start Portal)
uint8_t RFIDReader = RFID_READER;                                           // RFID Reader (0:Disabled / 1:Enabled / 2:Enable One / 3:Learn / 4:Delete / 5:Delete All / 6: Remote via OCPP)
#if FAKE_RFID
uint8_t Show_RFID = 0;
#endif

EnableC2_t EnableC2 = ENABLE_C2;                                            // Contactor C2
uint16_t maxTemp = MAX_TEMPERATURE;

Meter MainsMeter(MAINS_METER, MAINS_METER_ADDRESS, COMM_TIMEOUT);
Meter EVMeter(EV_METER, EV_METER_ADDRESS, COMM_EVTIMEOUT);
uint8_t Nr_Of_Phases_Charging = 0;                                          // 0 = Undetected, 1,2,3 = nr of phases that was detected at the start of this charging session
Single_Phase_t Switching_To_Single_Phase = FALSE;

uint8_t State = STATE_A;
uint8_t ErrorFlags = NO_ERROR;
uint8_t NextState;
uint8_t pilot;
uint8_t prev_pilot;

uint16_t MaxCapacity;                                                       // Cable limit (A) (limited by the wire in the charge cable, set automatically, or manually if Config=Fixed Cable)
uint16_t ChargeCurrent;                                                     // Calculated Charge Current (Amps *10)
uint16_t OverrideCurrent = 0;                                               // Temporary assigned current (Amps *10) (modbus)
int16_t Isum = 0;                                                           // Sum of all measured Phases (Amps *10) (can be negative)

// Load Balance variables
int16_t IsetBalanced = 0;                                                   // Max calculated current (Amps *10) available for all EVSE's
uint16_t Balanced[NR_EVSES] = {0, 0, 0, 0, 0, 0, 0, 0};                     // Amps value per EVSE
uint16_t BalancedMax[NR_EVSES] = {0, 0, 0, 0, 0, 0, 0, 0};                  // Max Amps value per EVSE
uint8_t BalancedState[NR_EVSES] = {0, 0, 0, 0, 0, 0, 0, 0};                 // State of all EVSE's 0=not active (state A), 1=charge request (State B), 2= Charging (State C)
uint16_t BalancedError[NR_EVSES] = {0, 0, 0, 0, 0, 0, 0, 0};                // Error state of EVSE

struct {
    uint8_t Online;
    uint8_t ConfigChanged;
    uint8_t EVMeter;
    uint8_t EVAddress;
    uint8_t MinCurrent;     // 0.1A
    uint8_t Phases;
    uint32_t Timer;         // 1s
    uint32_t IntTimer;      // 1s
    uint16_t SolarTimer;    // 1s
    uint8_t Mode;
} Node[NR_EVSES] = {                                                        // 0: Master / 1: Node 1 ...
   /*         Config   EV     EV       Min      Used    Charge Interval Solar *          // Interval Time   : last Charge time, reset when not charging
    * Online, Changed, Meter, Address, Current, Phases,  Timer,  Timer, Timer, Mode */   // Min Current     : minimal measured current per phase the EV consumes when starting to charge @ 6A (can be lower then 6A)
    {      1,       0,     0,       0,       0,      0,      0,      0,     0,    0 },   // Used Phases     : detected nr of phases when starting to charge (works with configured EVmeter meter, and might work with sensorbox)
    {      0,       1,     0,       0,       0,      0,      0,      0,     0,    0 },
    {      0,       1,     0,       0,       0,      0,      0,      0,     0,    0 },
    {      0,       1,     0,       0,       0,      0,      0,      0,     0,    0 },    
    {      0,       1,     0,       0,       0,      0,      0,      0,     0,    0 },
    {      0,       1,     0,       0,       0,      0,      0,      0,     0,    0 },
    {      0,       1,     0,       0,       0,      0,      0,      0,     0,    0 },
    {      0,       1,     0,       0,       0,      0,      0,      0,     0,    0 }            
};

uint8_t lock1 = 0, lock2 = 1;
uint16_t BacklightTimer = 0;                                                // Backlight timer (sec)
uint8_t BacklightSet = 0;
uint8_t LCDTimer = 0;
uint8_t AccessTimer = 0;
int8_t TempEVSE = 0;                                                        // Temperature EVSE in deg C (-50 to +125)
uint8_t ButtonState = 0x07;                                                 // Holds latest push Buttons state (LSB 2:0)
uint8_t OldButtonState = 0x07;                                              // Holds previous push Buttons state (LSB 2:0)
uint8_t LCDNav = 0;
uint8_t SubMenu = 0;
uint32_t ScrollTimer = 0;
uint8_t ChargeDelay = 0;                                                    // Delays charging at least 60 seconds in case of not enough current available.
uint8_t C1Timer = 0;
uint8_t ModemStage = 0;                                                     // 0: Modem states will be executed when Modem is enabled 1: Modem stages will be skipped, as SoC is already extracted
int8_t DisconnectTimeCounter = -1;                                          // Count for how long we're disconnected, so we can more reliably throw disconnect event. -1 means counter is disabled
uint8_t ToModemWaitStateTimer = 0;                                          // Timer used from STATE_MODEM_REQUEST to STATE_MODEM_WAIT
uint8_t ToModemDoneStateTimer = 0;                                          // Timer used from STATE_MODEM_WAIT to STATE_MODEM_DONE
uint8_t LeaveModemDoneStateTimer = 0;                                       // Timer used from STATE_MODEM_DONE to other, usually STATE_B
uint8_t LeaveModemDeniedStateTimer = 0;                                     // Timer used from STATE_MODEM_DENIED to STATE_B to re-try authentication
uint8_t NoCurrent = 0;                                                      // counts overcurrent situations.
uint8_t TestState = 0;
uint8_t ModbusRequest = 0;                                                  // Flag to request Modbus information
uint8_t NodeNewMode = 0;
uint8_t Access_bit = 0;                                                     // 0:No Access 1:Access to SmartEVSE
uint16_t CardOffset = CARD_OFFSET;                                          // RFID card used in Enable One mode

uint8_t ConfigChanged = 0;
uint8_t GridActive = 0;                                                     // When the CT's are used on Sensorbox2, it enables the GRID menu option.

uint16_t SolarStopTimer = 0;
uint8_t RFIDstatus = 0;
bool PilotDisconnected = false;
uint8_t PilotDisconnectTime = 0;                                            // Time the Control Pilot line should be disconnected (Sec)

uint8_t ActivationMode = 0, ActivationTimer = 0;
volatile uint16_t adcsample = 0;
volatile uint16_t ADCsamples[25];                                           // declared volatile, as they are used in a ISR
volatile uint8_t sampleidx = 0;
char str[20];

int phasesLastUpdate = 0;
bool phasesLastUpdateFlag = false;
int16_t IrmsOriginal[3]={0, 0, 0};
int homeBatteryCurrent = 0;
int homeBatteryLastUpdate = 0; // Time in milliseconds
// set by EXTERNAL logic through MQTT/REST to indicate cheap tariffs ahead until unix time indicated
uint8_t ColorOff[3] = {0, 0, 0};          // off
uint8_t ColorNormal[3] = {0, 255, 0};   // Green
uint8_t ColorSmart[3] = {0, 255, 0};    // Green
uint8_t ColorSolar[3] = {255, 170, 0};    // Orange
uint8_t ColorCustom[3] = {0, 0, 255};    // Blue

//#define FW_UPDATE_DELAY 30        //DINGO TODO                                            // time between detection of new version and actual update in seconds
#define FW_UPDATE_DELAY 3600                                                    // time between detection of new version and actual update in seconds
uint16_t firmwareUpdateTimer = 0;                                               // timer for firmware updates in seconds, max 0xffff = approx 18 hours
                                                                                // 0 means timer inactive
                                                                                // 0 < timer < FW_UPDATE_DELAY means we are in countdown for an actual update
                                                                                // FW_UPDATE_DELAY <= timer <= 0xffff means we are in countdown for checking
                                                                                //                                              whether an update is necessary

#if ENABLE_OCPP
uint8_t OcppMode = OCPP_MODE; //OCPP Client mode. 0:Disable / 1:Enable

unsigned char OcppRfidUuid [7];
size_t OcppRfidUuidLen;
unsigned long OcppLastRfidUpdate;
unsigned long OcppTrackLastRfidUpdate;

bool OcppForcesLock = false;
std::shared_ptr<MicroOcpp::Configuration> OcppUnlockConnectorOnEVSideDisconnect; // OCPP Config for RFID-based transactions: if false, demand same RFID card again to unlock connector
std::shared_ptr<MicroOcpp::Transaction> OcppLockingTx; // Transaction which locks connector until same RFID card is presented again

bool OcppTrackPermitsCharge = false;
bool OcppTrackAccessBit = false;
uint8_t OcppTrackCPvoltage = PILOT_NOK; //track positive part of CP signal for OCPP transaction logic
MicroOcpp::MOcppMongooseClient *OcppWsClient;

float OcppCurrentLimit = -1.f; // Negative value: no OCPP limit defined

unsigned long OcppStopReadingSyncTime; // Stop value synchronization: delay StopTransaction by a few seconds so it reports an accurate energy reading

bool OcppDefinedTxNotification;
MicroOcpp::TxNotification OcppTrackTxNotification;
unsigned long OcppLastTxNotification;
#endif //ENABLE_OCPP


#if SMARTEVSE_VERSION == 3
// Some low level stuff here to setup the ADC, and perform the conversion.
//
//
uint16_t IRAM_ATTR local_adc1_read(int channel) {
    uint16_t adc_value;

    SENS.sar_read_ctrl.sar1_dig_force = 0;                      // switch SARADC into RTC channel 
    SENS.sar_meas_wait2.force_xpd_sar = SENS_FORCE_XPD_SAR_PU;  // adc_power_on
    RTCIO.hall_sens.xpd_hall = false;                           // disable other peripherals
    
    //adc_ll_amp_disable()  // Close ADC AMP module if don't use it for power save.
    SENS.sar_meas_wait2.force_xpd_amp = SENS_FORCE_XPD_AMP_PD;  // channel is set in the convert function
    // disable FSM, it's only used by the LNA.
    SENS.sar_meas_ctrl.amp_rst_fb_fsm = 0; 
    SENS.sar_meas_ctrl.amp_short_ref_fsm = 0;
    SENS.sar_meas_ctrl.amp_short_ref_gnd_fsm = 0;
    SENS.sar_meas_wait1.sar_amp_wait1 = 1;
    SENS.sar_meas_wait1.sar_amp_wait2 = 1;
    SENS.sar_meas_wait2.sar_amp_wait3 = 1; 

    // adc_hal_set_controller(ADC_NUM_1, ADC_CTRL_RTC);         //Set controller
    // see esp-idf/components/hal/esp32/include/hal/adc_ll.h
    SENS.sar_read_ctrl.sar1_dig_force       = 0;                // 1: Select digital control;       0: Select RTC control.
    SENS.sar_meas_start1.meas1_start_force  = 1;                // 1: SW control RTC ADC start;     0: ULP control RTC ADC start.
    SENS.sar_meas_start1.sar1_en_pad_force  = 1;                // 1: SW control RTC ADC bit map;   0: ULP control RTC ADC bit map;
    SENS.sar_touch_ctrl1.xpd_hall_force     = 1;                // 1: SW control HALL power;        0: ULP FSM control HALL power.
    SENS.sar_touch_ctrl1.hall_phase_force   = 1;                // 1: SW control HALL phase;        0: ULP FSM control HALL phase.

    // adc_hal_convert(ADC_NUM_1, channel, &adc_value);
    // see esp-idf/components/hal/esp32/include/hal/adc_ll.h
    SENS.sar_meas_start1.sar1_en_pad = (1 << channel);          // select ADC channel to sample on
    while (SENS.sar_slave_addr1.meas_status != 0);              // wait for conversion to be idle (blocking)
    SENS.sar_meas_start1.meas1_start_sar = 0;         
    SENS.sar_meas_start1.meas1_start_sar = 1;                   // start ADC conversion
    while (SENS.sar_meas_start1.meas1_done_sar == 0);           // wait (blocking) for conversion to finish
    adc_value = SENS.sar_meas_start1.meas1_data_sar;            // read ADC value from register

    return adc_value;
}



// CP pin low to high transition ISR
//
//
void IRAM_ATTR onCPpulse() {

  // reset timer, these functions are in IRAM !
  timerWrite(timerA, 0);                                        
  timerAlarmEnable(timerA);
}



// Timer interrupt handler
// in STATE A this is called every 1ms (autoreload)
// in STATE B/C there is a PWM signal, and the Alarm is set to 5% after the low-> high transition of the PWM signal
void IRAM_ATTR onTimerA() {

  RTC_ENTER_CRITICAL();
  adcsample = local_adc1_read(ADC1_CHANNEL_3);

  RTC_EXIT_CRITICAL();

  ADCsamples[sampleidx++] = adcsample;
  if (sampleidx == 25) sampleidx = 0;
}

#endif //SMARTEVSE_VERSION

// --------------------------- END of ISR's -----------------------------------------------------

// Blink the RGB LED and LCD Backlight.
//
// NOTE: need to add multiple colour schemes 
//
// Task is called every 10ms
void BlinkLed(void * parameter) {
    uint8_t LcdPwm = 0;
    uint8_t RedPwm = 0, GreenPwm = 0, BluePwm = 0;
    uint8_t LedCount = 0;                                                   // Raw Counter before being converted to PWM value
    unsigned int LedPwm = 0;                                                // PWM value 0-255

    while(1) 
    {
        // Backlight LCD
        if (BacklightTimer > 1 && BacklightSet != 1) {                      // Enable LCD backlight at max brightness
                                                                            // start only when fully off(0) or when we are dimming the backlight(2)
            LcdPwm = LCD_BRIGHTNESS;
            ledcWrite(LCD_CHANNEL, LcdPwm);
            BacklightSet = 1;                                               // 1: we have set the backlight to max brightness
        } 
        
        if (BacklightTimer == 1 && LcdPwm >= 3) {                           // Last second of Backlight
            LcdPwm -= 3;
            ledcWrite(LCD_CHANNEL, ease8InOutQuad(LcdPwm));                 // fade out
            BacklightSet = 2;                                               // 2: we are dimming the backlight
        }
                                                                            // Note: could be simplified by removing following code if LCD_BRIGHTNESS is multiple of 3                                                               
        if (BacklightTimer == 0 && BacklightSet) {                          // End of LCD backlight
            ledcWrite(LCD_CHANNEL, 0);                                      // switch off LED PWM
            BacklightSet = 0;                                               // 0: backlight fully off
        }

        // RGB LED
        if (ErrorFlags || ChargeDelay) {

            if (ErrorFlags & (RCM_TRIPPED | CT_NOCOMM | EV_NOCOMM) ) {
                LedCount += 20;                                                 // Very rapid flashing, RCD tripped or no Serial Communication.
                if (LedCount > 128) LedPwm = ERROR_LED_BRIGHTNESS;              // Red LED 50% of time on, full brightness
                else LedPwm = 0;
                RedPwm = LedPwm;
                GreenPwm = 0;
                BluePwm = 0;
            } else {                                                            // Waiting for Solar power or not enough current to start charging
                LedCount += 2;                                                  // Slow blinking.
                if (LedCount > 230) LedPwm = WAITING_LED_BRIGHTNESS;            // LED 10% of time on, full brightness
                else LedPwm = 0;

                if (CustomButton) {                                             // Blue for Custom, unless configured otherwise
                    RedPwm = LedPwm * ColorCustom[0] / 255;
                    GreenPwm = LedPwm * ColorCustom[1] / 255;
                    BluePwm = LedPwm * ColorCustom[2] / 255;
                } else if (Mode == MODE_SOLAR) {                                // Orange for Solar, unless configured otherwise
                    RedPwm = LedPwm * ColorSolar[0] / 255;
                    GreenPwm = LedPwm * ColorSolar[1] / 255;
                    BluePwm = LedPwm * ColorSolar[2] / 255;
                } else if (Mode == MODE_SMART) {                                // Green for Smart, unless configured otherwise
                    RedPwm = LedPwm * ColorSmart[0] / 255;
                    GreenPwm = LedPwm * ColorSmart[1] / 255;
                    BluePwm = LedPwm * ColorSmart[2] / 255;
                } else {                                                        // Green for Normal, unless configured otherwise
                    RedPwm = LedPwm * ColorNormal[0] / 255;
                    GreenPwm = LedPwm * ColorNormal[1] / 255;
                    BluePwm = LedPwm * ColorNormal[2] / 255;
                }    
            }

#if ENABLE_OCPP
        } else if (OcppMode && (RFIDReader == 6 || RFIDReader == 0) &&
                    millis() - OcppLastRfidUpdate < 200) {
            RedPwm = 128;
            GreenPwm = 128;
            BluePwm = 128;
        } else if (OcppMode && (RFIDReader == 6 || RFIDReader == 0) &&
                    millis() - OcppLastTxNotification < 1000 && OcppTrackTxNotification == MicroOcpp::TxNotification::Authorized) {
            RedPwm = 0;
            GreenPwm = 255;
            BluePwm = 0;
        } else if (OcppMode && (RFIDReader == 6 || RFIDReader == 0) &&
                    millis() - OcppLastTxNotification < 2000 && (OcppTrackTxNotification == MicroOcpp::TxNotification::AuthorizationRejected ||
                                                                 OcppTrackTxNotification == MicroOcpp::TxNotification::DeAuthorized ||
                                                                 OcppTrackTxNotification == MicroOcpp::TxNotification::ReservationConflict)) {
            RedPwm = 255;
            GreenPwm = 0;
            BluePwm = 0;
        } else if (OcppMode && (RFIDReader == 6 || RFIDReader == 0) &&
                    millis() - OcppLastTxNotification < 300 && (OcppTrackTxNotification == MicroOcpp::TxNotification::AuthorizationTimeout ||
                                                                OcppTrackTxNotification == MicroOcpp::TxNotification::ConnectionTimeout)) {
            RedPwm = 255;
            GreenPwm = 0;
            BluePwm = 0;
        } else if (OcppMode && (RFIDReader == 6 || RFIDReader == 0) &&
                    getChargePointStatus() == ChargePointStatus_Reserved) {
            RedPwm = 196;
            GreenPwm = 64;
            BluePwm = 0;
        } else if (OcppMode && (RFIDReader == 6 || RFIDReader == 0) &&
                    (getChargePointStatus() == ChargePointStatus_Unavailable ||
                     getChargePointStatus() == ChargePointStatus_Faulted)) {
            RedPwm = 255;
            GreenPwm = 0;
            BluePwm = 0;
#endif //ENABLE_OCPP
        } else if (Access_bit == 0 && CustomButton) {
            RedPwm = ColorCustom[0];
            GreenPwm = ColorCustom[1];
            BluePwm = ColorCustom[2];
        } else if (Access_bit == 0 || State == STATE_MODEM_DENIED) {
            RedPwm = ColorOff[0];
            GreenPwm = ColorOff[1];
            BluePwm = ColorOff[2];
        } else {                                                                // State A, B or C
    
            if (State == STATE_A) {
                LedPwm = STATE_A_LED_BRIGHTNESS;                                // STATE A, LED on (dimmed)
            
            } else if (State == STATE_B || State == STATE_B1 || State == STATE_MODEM_REQUEST || State == STATE_MODEM_WAIT) {
                LedPwm = STATE_B_LED_BRIGHTNESS;                                // STATE B, LED on (full brightness)
                LedCount = 128;                                                 // When switching to STATE C, start at full brightness

            } else if (State == STATE_C) {                                      
                if (Mode == MODE_SOLAR) LedCount ++;                            // Slower fading (Solar mode)
                else LedCount += 2;                                             // Faster fading (Smart mode)
                LedPwm = ease8InOutQuad(triwave8(LedCount));                    // pre calculate new LedPwm value
            }

            if (CustomButton) {                                             // Blue for Custom, unless configured otherwise
                RedPwm = LedPwm * ColorCustom[0] / 255;
                GreenPwm = LedPwm * ColorCustom[1] / 255;
                BluePwm = LedPwm * ColorCustom[2] / 255;
            } else if (Mode == MODE_SOLAR) {                                // Orange for Solar, unless configured otherwise
                RedPwm = LedPwm * ColorSolar[0] / 255;
                GreenPwm = LedPwm * ColorSolar[1] / 255;
                BluePwm = LedPwm * ColorSolar[2] / 255;
            } else if (Mode == MODE_SMART) {                                // Green for Smart, unless configured otherwise
                RedPwm = LedPwm * ColorSmart[0] / 255;
                GreenPwm = LedPwm * ColorSmart[1] / 255;
                BluePwm = LedPwm * ColorSmart[2] / 255;
            } else {                                                        // Green for Normal, unless configured otherwise
                RedPwm = LedPwm * ColorNormal[0] / 255;
                GreenPwm = LedPwm * ColorNormal[1] / 255;
                BluePwm = LedPwm * ColorNormal[2] / 255;
            }    

        }
        ledcWrite(RED_CHANNEL, RedPwm);
        ledcWrite(GREEN_CHANNEL, GreenPwm);
        ledcWrite(BLUE_CHANNEL, BluePwm);

        // Pause the task for 10ms
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }  // while(1) loop 
}


// Set Charge Current 
// Current in Amps * 10 (160 = 16A)
void SetCurrent(uint16_t current) {
    uint32_t DutyCycle;

    if ((current >= (MIN_CURRENT * 10)) && (current <= 510)) DutyCycle = current / 0.6;
                                                                            // calculate DutyCycle from current
    else if ((current > 510) && (current <= 800)) DutyCycle = (current / 2.5) + 640;
    else DutyCycle = 100;                                                   // invalid, use 6A
    DutyCycle = DutyCycle * 1024 / 1000;                                    // conversion to 1024 = 100%
    SetCPDuty(DutyCycle);
}

// Write duty cycle to pin
// Value in range 0 (0% duty) to 1024 (100% duty)
void SetCPDuty(uint32_t DutyCycle){
    ledcWrite(CP_CHANNEL, DutyCycle);                                       // update PWM signal
    CurrentPWM = DutyCycle;
}


#if ENABLE_OCPP
// Inverse function of SetCurrent (for monitoring and debugging purposes)
uint16_t GetCurrent() {
    uint32_t DutyCycle = CurrentPWM;

    if (DutyCycle < 102) {
        return 0; //PWM off or ISO15118 modem enabled
    } else if (DutyCycle < 870) {
        return (DutyCycle * 1000 / 1024) * 0.6 + 1; // invert duty cycle formula + fixed rounding error correction
    } else if (DutyCycle <= 983) {
        return ((DutyCycle * 1000 / 1024)- 640) * 2.5 + 3; // invert duty cycle formula + fixed rounding error correction
    } else {
        return 0; //constant +12V
    }
}
#endif //ENABLE_OCPP


#if SMARTEVSE_VERSION == 3
// Sample the Temperature sensor.
//
signed char TemperatureSensor() {
    uint32_t sample, voltage;
    signed char Temperature;

    RTC_ENTER_CRITICAL();
    // Sample Temperature Sensor
    sample = local_adc1_read(ADC1_CHANNEL_0);
    RTC_EXIT_CRITICAL();

    // voltage range is from 0-2200mV 
    voltage = esp_adc_cal_raw_to_voltage(sample, adc_chars_Temperature);

    // The MCP9700A temperature sensor outputs 500mV at 0C, and has a 10mV/C change in output voltage.
    // so 750mV is 25C, 400mV = -10C
    Temperature = (signed int)(voltage - 500)/10;
    //_LOG_A("\nTemp: %i C (%u mV) ", Temperature , voltage);
    
    return Temperature;
}

// Sample the Proximity Pin, and determine the maximum current the cable can handle.
//
void ProximityPin() {
    uint32_t sample, voltage;

    RTC_ENTER_CRITICAL();
    // Sample Proximity Pilot (PP)
    sample = local_adc1_read(ADC1_CHANNEL_6);
    RTC_EXIT_CRITICAL();

    voltage = esp_adc_cal_raw_to_voltage(sample, adc_chars_PP);

    if (!Config) {                                                          // Configuration (0:Socket / 1:Fixed Cable)
        //socket
        _LOG_A("PP pin: %u (%u mV)\n", sample, voltage);
    } else {
        //fixed cable
        _LOG_A("PP pin: %u (%u mV) (warning: fixed cable configured so PP probably disconnected, making this reading void)\n", sample, voltage);
    }

    MaxCapacity = 13;                                                       // No resistor, Max cable current = 13A
    if ((voltage > 1200) && (voltage < 1400)) MaxCapacity = 16;             // Max cable current = 16A	680R -> should be around 1.3V
    if ((voltage > 500) && (voltage < 700)) MaxCapacity = 32;               // Max cable current = 32A	220R -> should be around 0.6V
    if ((voltage > 200) && (voltage < 400)) MaxCapacity = 63;               // Max cable current = 63A	100R -> should be around 0.3V

    if (Config) MaxCapacity = MaxCurrent;                                   // Override with MaxCurrent when Fixed Cable is used.
}


// Determine the state of the Pilot signal
//
uint8_t Pilot() {

    uint32_t sample, Min = 3300, Max = 0;
    uint32_t voltage;
    uint8_t n;

    // calculate Min/Max of last 25 CP measurements
    for (n=0 ; n<25 ;n++) {
        sample = ADCsamples[n];
        voltage = esp_adc_cal_raw_to_voltage( sample, adc_chars_CP);        // convert adc reading to voltage
        if (voltage < Min) Min = voltage;                                   // store lowest value
        if (voltage > Max) Max = voltage;                                   // store highest value
    }    
    //_LOG_A("min:%u max:%u\n",Min ,Max);

    // test Min/Max against fixed levels
    if (Min >= 3055 ) return PILOT_12V;                                     // Pilot at 12V (min 11.0V)
    if ((Min >= 2735) && (Max < 3055)) return PILOT_9V;                     // Pilot at 9V
    if ((Min >= 2400) && (Max < 2735)) return PILOT_6V;                     // Pilot at 6V
    if ((Min >= 2000) && (Max < 2400)) return PILOT_3V;                     // Pilot at 3V
    if ((Min > 100) && (Max < 300)) return PILOT_DIODE;                     // Diode Check OK
    return PILOT_NOK;                                                       // Pilot NOT ok
}
#endif


/**
 * Get name of a state
 *
 * @param uint8_t State
 * @return uint8_t[] Name
 */
const char * getStateName(uint8_t StateCode) {
    if(StateCode < 15) return StrStateName[StateCode];
    else return "NOSTATE";
}


const char * getStateNameWeb(uint8_t StateCode) {
    if(StateCode < 15) return StrStateNameWeb[StateCode];
    else return "NOSTATE";    
}


uint8_t getErrorId(uint8_t ErrorCode) {
    uint8_t count = 0;
    //find the error bit that is set
    while (ErrorCode) {
        count++;
        ErrorCode = ErrorCode >> 1;
    }    
    return count;
}


const char * getErrorNameWeb(uint8_t ErrorCode) {
    uint8_t count = 0;
    count = getErrorId(ErrorCode);
    if(count < 9) return StrErrorNameWeb[count];
    else return "Multiple Errors";
}

/**
 * Set EVSE mode
 * 
 * @param uint8_t Mode
 */
void setMode(uint8_t NewMode) {
    // If mainsmeter disabled we can only run in Normal Mode
    if (!MainsMeter.Type && NewMode != MODE_NORMAL)
        return;

    // Take care of extra conditionals/checks for custom features
    setAccess(!DelayedStartTime.epoch2); //if DelayedStartTime not zero then we are Delayed Charging
    if (NewMode == MODE_SOLAR) {
        // Reset OverrideCurrent if mode is SOLAR
        OverrideCurrent = 0;
    }

    // when switching modes, we just keep charging at the phases we were charging at;
    // it's only the regulation algorithm that is changing...
    // EXCEPT when EnableC2 == Solar Off, because we would expect C2 to be off when in Solar Mode and EnableC2 == Solar Off
    // and also the other way around, multiple phases might be wanted when changing from Solar to Normal or Smart
    bool switchOnLater = false;
    if (EnableC2 == SOLAR_OFF) {
        if ((Mode != MODE_SOLAR && NewMode == MODE_SOLAR) || (Mode == MODE_SOLAR && NewMode != MODE_SOLAR)) {
            //we are switching from non-solar to solar
            //since we EnableC2 == SOLAR_OFF C2 is turned On now, and should be turned off
            setAccess(0);                                                       //switch to OFF
            switchOnLater = true;
        }
    }

#if MQTT
    // Update MQTT faster
    lastMqttUpdate = 10;
#endif

    if (NewMode == MODE_SMART) {
        ErrorFlags &= ~(NO_SUN | LESS_6A);                                      // Clear All errors
        setSolarStopTimer(0);                                                   // Also make sure the SolarTimer is disabled.
        MaxSumMainsTimer = 0;
    }
    ChargeDelay = 0;                                                            // Clear any Chargedelay
    BacklightTimer = BACKLIGHT;                                                 // Backlight ON
    if (Mode != NewMode) NodeNewMode = NewMode + 1;
    Mode = NewMode;    

    if (switchOnLater)
        setAccess(1);

    //make mode and start/stoptimes persistent on reboot
    if (preferences.begin("settings", false) ) {                        //false = write mode
        preferences.putUChar("Mode", Mode);
        preferences.putULong("DelayedStartTim", DelayedStartTime.epoch2); //epoch2 only needs 4 bytes
        preferences.putULong("DelayedStopTime", DelayedStopTime.epoch2);   //epoch2 only needs 4 bytes
        preferences.putUShort("DelayedRepeat", DelayedRepeat);
        preferences.end();
    }
}

/**
 * Checks all parameters to determine whether
 * we are going to force single phase charging
 * Returns true if we are going to do single phase charging
 * Returns false if we are going to do (traditional) 3 phase charing
 * This is only relevant on a 3f mains and 3f car installation!
 * 1f car will always charge 1f undetermined by CONTACTOR2
 */
uint8_t Force_Single_Phase_Charging() {                                         // abbreviated to FSPC
    switch (EnableC2) {
        case NOT_PRESENT:                                                       //no use trying to switch a contactor on that is not present
        case ALWAYS_OFF:
            return 1;
        case SOLAR_OFF:
            return (Mode == MODE_SOLAR);
        case AUTO:
        case ALWAYS_ON:
            return 0;   //3f charging
    }
    //in case we don't know, stick to 3f charging
    return 0;
}

void setStatePowerUnavailable(void) {
    if (State == STATE_A)
       return;
    //State changes between A,B,C,D are caused by EV or by the user
    //State changes between x1 and x2 are created by the EVSE
    //State changes between x1 and x2 indicate availability (x2) of unavailability (x1) of power supply to the EV
    if (State == STATE_C) setState(STATE_C1);                       // If we are charging, tell EV to stop charging
    else if (State != STATE_C1) setState(STATE_B1);                 // If we are not in State C1, switch to State B1
}

void setState(uint8_t NewState) {
    if (State != NewState) {
        char Str[50];
        snprintf(Str, sizeof(Str), "%02d:%02d:%02d STATE %s -> %s\n",timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, getStateName(State), getStateName(NewState) );

        _LOG_A("%s",Str);
    }

    switch (NewState) {
        case STATE_B1:
            if (!ChargeDelay) ChargeDelay = 3;                                  // When entering State B1, wait at least 3 seconds before switching to another state.
            if (State != STATE_B1 && State != STATE_B && !PilotDisconnected) {
                PILOT_DISCONNECTED;
                PilotDisconnected = true;
                PilotDisconnectTime = 5;                                       // Set PilotDisconnectTime to 5 seconds

                _LOG_A("Pilot Disconnected\n");
            }
            // fall through
        case STATE_A:                                                           // State A1
            CONTACTOR1_OFF;  
            CONTACTOR2_OFF;  
            SetCPDuty(1024);                                                    // PWM off,  channel 0, duty cycle 100%
            timerAlarmWrite(timerA, PWM_100, true);                             // Alarm every 1ms, auto reload 
            if (NewState == STATE_A) {
                ErrorFlags &= ~NO_SUN;
                ErrorFlags &= ~LESS_6A;
                ChargeDelay = 0;
                Switching_To_Single_Phase = FALSE;
                // Reset Node
                Node[0].Timer = 0;
                Node[0].IntTimer = 0;
                Node[0].Phases = 0;
                Node[0].MinCurrent = 0;                                         // Clear ChargeDelay when disconnected.
            }

#if MODEM
            if (DisconnectTimeCounter == -1){
                DisconnectTimeCounter = 0;                                      // Start counting disconnect time. If longer than 60 seconds, throw DisconnectEvent
            }
            break;
        case STATE_MODEM_REQUEST: // After overriding PWM, and resetting the safe state is 10% PWM. To make sure communication recovers after going to normal, we do this. Ugly and temporary
            ToModemWaitStateTimer = 5;
            DisconnectTimeCounter = -1;                                         // Disable Disconnect timer. Car is connected
            SetCPDuty(1024);
            CONTACTOR1_OFF;
            CONTACTOR2_OFF;
            break;
        case STATE_MODEM_WAIT: 
            SetCPDuty(50);
            ToModemDoneStateTimer = 60;
            break;
        case STATE_MODEM_DONE:  // This state is reached via STATE_MODEM_WAIT after 60s (timeout condition, nothing received) or after REST request (success, shortcut to immediate charging).
            CP_OFF;
            DisconnectTimeCounter = -1;                                         // Disable Disconnect timer. Car is connected
            LeaveModemDoneStateTimer = 5;                                       // Disconnect CP for 5 seconds, restart charging cycle but this time without the modem steps.
#endif
            break;
        case STATE_B:
#if MODEM
            CP_ON;
            DisconnectTimeCounter = -1;                                         // Disable Disconnect timer. Car is connected
#endif
            CONTACTOR1_OFF;
            CONTACTOR2_OFF;
            timerAlarmWrite(timerA, PWM_95, false);                             // Enable Timer alarm, set to diode test (95%)
            SetCurrent(ChargeCurrent);                                          // Enable PWM
            break;      
        case STATE_C:                                                           // State C2
            ActivationMode = 255;                                               // Disable ActivationMode

            if (Switching_To_Single_Phase == GOING_TO_SWITCH) {
                    CONTACTOR2_OFF;
                    setSolarStopTimer(0); //TODO still needed? now we switched contactor2 off, review if we need to stop solar charging
                    MaxSumMainsTimer = 0;
                    //Nr_Of_Phases_Charging = 1; this will be detected automatically
                    Switching_To_Single_Phase = AFTER_SWITCH;                   // we finished the switching process,
                                                                                // BUT we don't know which is the single phase
            }

            CONTACTOR1_ON;
            if (!Force_Single_Phase_Charging() && Switching_To_Single_Phase != AFTER_SWITCH) {                               // in AUTO mode we start with 3phases
                CONTACTOR2_ON;                                                  // Contactor2 ON
            }
            LCDTimer = 0;
            break;
        case STATE_C1:
            SetCPDuty(1024);                                                    // PWM off,  channel 0, duty cycle 100%
            timerAlarmWrite(timerA, PWM_100, true);                             // Alarm every 1ms, auto reload 
                                                                                // EV should detect and stop charging within 3 seconds
            C1Timer = 6;                                                        // Wait maximum 6 seconds, before forcing the contactor off.
            ChargeDelay = 15;
            break;
        default:
            break;
    }
    
    BalancedState[0] = NewState;
    State = NewState;

#if MQTT
    // Update MQTT faster
    lastMqttUpdate = 10;
#endif

    // BacklightTimer = BACKLIGHT;                                                 // Backlight ON
}

void setAccess(bool Access) {
    Access_bit = Access;
    if (Access == 0) {
        //TODO:setStatePowerUnavailable() ?
        if (State == STATE_C) setState(STATE_C1);                               // Determine where to switch to.
        else if (State != STATE_C1 && (State == STATE_B || State == STATE_MODEM_REQUEST || State == STATE_MODEM_WAIT || State == STATE_MODEM_DONE || State == STATE_MODEM_DENIED)) setState(STATE_B1);
    }

    //make mode and start/stoptimes persistent on reboot
    if (preferences.begin("settings", false) ) {                        //false = write mode
        preferences.putUChar("Access", Access_bit);
        preferences.putUShort("CardOffs16", CardOffset);
        preferences.end();
    }

#if MQTT
    // Update MQTT faster
    lastMqttUpdate = 10;
#endif
}

/**
 * Returns the known battery charge rate if the data is not too old.
 * Returns 0 if data is too old.
 * A positive number means charging, a negative number means discharging --> this means the inverse must be used for calculations
 * 
 * Example:
 * homeBatteryCharge == 1000 --> Battery is charging using Solar
 * P1 = -500 --> Solar injection to the net but nut sufficient for charging
 * 
 * If the P1 value is added with the inverse battery charge it will inform the EVSE logic there is enough Solar --> -500 + -1000 = -1500
 * 
 * Note: The user who is posting battery charge data should take this into account, meaning: if he wants a minimum home battery (dis)charge rate he should substract this from the value he is sending.
 */
// 
int getBatteryCurrent(void) {
    int currentTime = time(NULL) - 60; // The data should not be older than 1 minute
    
    if (Mode == MODE_SOLAR && homeBatteryLastUpdate > (currentTime)) {
        return homeBatteryCurrent;
    } else {
        homeBatteryCurrent = 0;
        homeBatteryLastUpdate = 0;
        return 0;
    }
}



// Is there at least 6A(configurable MinCurrent) available for a new EVSE?
// Look whether there would be place for one more EVSE if we could lower them all down to MinCurrent
// returns 1 if there is 6A available
// returns 0 if there is no current available
// only runs on the Master or when loadbalancing Disabled
char IsCurrentAvailable(void) {
    uint8_t n, ActiveEVSE = 0;
    int Baseload, Baseload_EV, TotalCurrent = 0;

    for (n = 0; n < NR_EVSES; n++) if (BalancedState[n] == STATE_C)             // must be in STATE_C
    {
        ActiveEVSE++;                                                           // Count nr of active (charging) EVSE's
        TotalCurrent += Balanced[n];                                            // Calculate total of all set charge currents
    }

    // Allow solar Charging if surplus current is above 'StartCurrent' (sum of all phases)
    // Charging will start after the timeout (chargedelay) period has ended
     // Only when StartCurrent configured or Node MinCurrent detected or Node inactive
    if (Mode == MODE_SOLAR) {                                                   // no active EVSE yet?
        if (ActiveEVSE == 0 && Isum >= ((signed int)StartCurrent *-10)) {
            _LOG_D("No current available StartCurrent line %d. ActiveEVSE=%i, TotalCurrent=%.1fA, StartCurrent=%iA, Isum=%.1fA, ImportCurrent=%iA.\n", __LINE__, ActiveEVSE, (float) TotalCurrent/10, StartCurrent, (float)Isum/10, ImportCurrent);
            return 0;
        }
        else if ((ActiveEVSE * MinCurrent * 10) > TotalCurrent) {               // check if we can split the available current between all active EVSE's
            _LOG_D("No current available TotalCurrent line %d. ActiveEVSE=%i, TotalCurrent=%.1fA, StartCurrent=%iA, Isum=%.1fA, ImportCurrent=%iA.\n", __LINE__, ActiveEVSE, (float) TotalCurrent/10, StartCurrent, (float)Isum/10, ImportCurrent);
            return 0;
        }
        else if (ActiveEVSE > 0 && Isum > ((signed int)ImportCurrent * 10) + TotalCurrent - (ActiveEVSE * MinCurrent * 10)) {
            _LOG_D("No current available Isum line %d. ActiveEVSE=%i, TotalCurrent=%.1fA, StartCurrent=%iA, Isum=%.1fA, ImportCurrent=%iA.\n", __LINE__, ActiveEVSE, (float) TotalCurrent/10, StartCurrent, (float)Isum/10, ImportCurrent);
            return 0;
        }
    }

    ActiveEVSE++;                                                           // Do calculations with one more EVSE
    if (ActiveEVSE > NR_EVSES) ActiveEVSE = NR_EVSES;
    Baseload = MainsMeter.Imeasured - TotalCurrent;                         // Calculate Baseload (load without any active EVSE)
    Baseload_EV = EVMeter.Imeasured - TotalCurrent;                         // Load on the EV subpanel excluding any active EVSE
    if (Baseload_EV < 0) Baseload_EV = 0;                                   // so Baseload_EV = 0 when no EVMeter installed

    // Check if the lowest charge current(6A) x ActiveEV's + baseload would be higher then the MaxMains.
    if (Mode != MODE_NORMAL && (ActiveEVSE * (MinCurrent * 10) + Baseload) > (MaxMains * 10)) {
        _LOG_D("No current available MaxMains line %d. ActiveEVSE=%i, Baseload=%.1fA, MinCurrent=%iA, MaxMains=%iA.\n", __LINE__, ActiveEVSE, (float) Baseload/10, MinCurrent, MaxMains);
        return 0;                                                           // Not enough current available!, return with error
    }
    if (((LoadBl == 0 && EVMeter.Type && Mode != MODE_NORMAL) || LoadBl == 1) // Conditions in which MaxCircuit has to be considered
        && ((ActiveEVSE * (MinCurrent * 10) + Baseload_EV) > (MaxCircuit * 10))) { // MaxCircuit is exceeded
        _LOG_D("No current available MaxCircuit line %d. ActiveEVSE=%i, Baseload_EV=%.1fA, MinCurrent=%iA, MaxCircuit=%iA.\n", __LINE__, ActiveEVSE, (float) Baseload_EV/10, MinCurrent, MaxCircuit);
        return 0;                                                           // Not enough current available!, return with error
    }
    //assume the current should be available on all 3 phases
    bool must_be_single_phase_charging = (EnableC2 == ALWAYS_OFF || (Mode == MODE_SOLAR && EnableC2 == SOLAR_OFF) ||
            (Mode == MODE_SOLAR && EnableC2 == AUTO && Switching_To_Single_Phase == AFTER_SWITCH));
    int Phases = must_be_single_phase_charging ? 1 : 3;
    if (Mode != MODE_NORMAL && MaxSumMains && ((Phases * ActiveEVSE * MinCurrent * 10) + Isum > MaxSumMains * 10)) {
        _LOG_D("No current available MaxSumMains line %d. ActiveEVSE=%i, MinCurrent=%iA, Isum=%.1fA, MaxSumMains=%iA.\n", __LINE__, ActiveEVSE, MinCurrent,  (float)Isum/10, MaxSumMains);
        return 0;                                                           // Not enough current available!, return with error
    }

// Use OCPP Smart Charging if Load Balancing is turned off
#if ENABLE_OCPP
    if (OcppMode &&                            // OCPP enabled
            !LoadBl &&                         // Internal LB disabled
            OcppCurrentLimit >= 0.f &&         // OCPP limit defined
            OcppCurrentLimit < MinCurrent) {  // OCPP suspends charging
        _LOG_D("OCPP Smart Charging suspends EVSE\n");
        return 0;
    }
#endif //ENABLE_OCPP

    _LOG_D("Current available checkpoint D. ActiveEVSE increased by one=%i, TotalCurrent=%.1fA, StartCurrent=%iA, Isum=%.1fA, ImportCurrent=%iA.\n", ActiveEVSE, (float) TotalCurrent/10, StartCurrent, (float)Isum/10, ImportCurrent);
    return 1;
}

// Set global var Nr_Of_Phases_Charging
// 0 = undetected, 1 - 3 nr of phases we are charging
// returns nr of phases we are charging, and 3 if undetected
int Set_Nr_of_Phases_Charging(void) {
    uint32_t Max_Charging_Prob = 0;
    uint32_t Charging_Prob=0;                                        // Per phase, the probability that Charging is done at this phase
    Nr_Of_Phases_Charging = 0;
#define THRESHOLD 40
#define BOTTOM_THRESHOLD 25
    _LOG_D("Detected Charging Phases: ChargeCurrent=%u, Balanced[0]=%u, IsetBalanced=%u.\n", ChargeCurrent, Balanced[0],IsetBalanced);
    for (int i=0; i<3; i++) {
        if (EVMeter.Type) {
            Charging_Prob = 10 * (abs(EVMeter.Irms[i] - IsetBalanced)) / IsetBalanced;  //100% means this phase is charging, 0% mwans not charging
                                                                                        //TODO does this work for the slaves too?
            _LOG_D("Trying to detect Charging Phases END EVMeter.Irms[%i]=%.1f A.\n", i, (float)EVMeter.Irms[i]/10);
        }
        Max_Charging_Prob = max(Charging_Prob, Max_Charging_Prob);

        //normalize percentages so they are in the range [0-100]
        if (Charging_Prob >= 200)
            Charging_Prob = 0;
        if (Charging_Prob > 100)
            Charging_Prob = 200 - Charging_Prob;
        _LOG_I("Detected Charging Phases: Charging_Prob[%i]=%i.\n", i, Charging_Prob);

        if (Charging_Prob == Max_Charging_Prob) {
            _LOG_D("Suspect I am charging at phase: L%i.\n", i+1);
            Nr_Of_Phases_Charging++;
        }
        else {
            if ( Charging_Prob <= BOTTOM_THRESHOLD ) {
                _LOG_D("Suspect I am NOT charging at phase: L%i.\n", i+1);
            }
            else {
                if ( Max_Charging_Prob - Charging_Prob <= THRESHOLD ) {
                    _LOG_D("Serious candidate for charging at phase: L%i.\n", i+1);
                    Nr_Of_Phases_Charging++;
                }
            }
        }
    }

    // sanity checks
    if (EnableC2 != AUTO && EnableC2 != NOT_PRESENT) {                         // no further sanity checks possible when AUTO or NOT_PRESENT
        if (Nr_Of_Phases_Charging != 1 && (EnableC2 == ALWAYS_OFF || (EnableC2 == SOLAR_OFF && Mode == MODE_SOLAR))) {
            _LOG_A("Error in detecting phases: EnableC2=%s and Nr_Of_Phases_Charging=%i.\n", StrEnableC2[EnableC2], Nr_Of_Phases_Charging);
            Nr_Of_Phases_Charging = 1;
            _LOG_A("Setting Nr_Of_Phases_Charging to 1.\n");
        }
        if (!Force_Single_Phase_Charging() && Nr_Of_Phases_Charging != 3) {//TODO 2phase charging very rare?
            _LOG_A("Possible error in detecting phases: EnableC2=%s and Nr_Of_Phases_Charging=%i.\n", StrEnableC2[EnableC2], Nr_Of_Phases_Charging);
        }
    }

    _LOG_A("Charging at %i phases.\n", Nr_Of_Phases_Charging);
    if (Nr_Of_Phases_Charging == 0)
        return 3;
    return Nr_Of_Phases_Charging;
}

// Calculates Balanced PWM current for each EVSE
// mod =0 normal
// mod =1 we have a new EVSE requesting to start charging.
// only runs on the Master or when loadbalancing Disabled
void CalcBalancedCurrent(char mod) {
    int Average, MaxBalanced, Idifference, Baseload_EV;
    int ActiveEVSE = 0;
    signed int IsumImport = 0;
    int ActiveMax = 0, TotalCurrent = 0, Baseload;
    char CurrentSet[NR_EVSES] = {0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t n;
    bool LimitedByMaxSumMains = false;

    // ############### first calculate some basic variables #################
    if (BalancedState[0] == STATE_C && MaxCurrent > MaxCapacity && !Config)
        ChargeCurrent = MaxCapacity * 10;
    else
        ChargeCurrent = MaxCurrent * 10;                                        // Instead use new variable ChargeCurrent.

// Use OCPP Smart Charging if Load Balancing is turned off
#if ENABLE_OCPP
    if (OcppMode &&                      // OCPP enabled
            !LoadBl &&                   // Internal LB disabled
            OcppCurrentLimit >= 0.f) {   // OCPP limit defined

        if (OcppCurrentLimit < MinCurrent) {
            ChargeCurrent = 0;
        } else {
            ChargeCurrent = std::min(ChargeCurrent, (uint16_t) (10.f * OcppCurrentLimit));
        }
    }
#endif //ENABLE_OCPP

    // Override current temporary if set
    if (OverrideCurrent)
        ChargeCurrent = OverrideCurrent;

    BalancedMax[0] = ChargeCurrent;
                                                                                // update BalancedMax[0] if the MAX current was adjusted using buttons or CLI
    for (n = 0; n < NR_EVSES; n++) if (BalancedState[n] == STATE_C) {
            ActiveEVSE++;                                                       // Count nr of Active (Charging) EVSE's
            ActiveMax += BalancedMax[n];                                        // Calculate total Max Amps for all active EVSEs
            TotalCurrent += Balanced[n];                                        // Calculate total of all set charge currents
    }

    _LOG_V("Checkpoint 1 Isetbalanced=%.1f A Imeasured=%.1f A MaxCircuit=%i Imeasured_EV=%.1f A, Battery Current = %.1f A, mode=%i.\n", (float)IsetBalanced/10, (float)MainsMeter.Imeasured/10, MaxCircuit, (float)EVMeter.Imeasured/10, (float)homeBatteryCurrent/10, Mode);

    Baseload_EV = EVMeter.Imeasured - TotalCurrent;                             // Calculate Baseload (load without any active EVSE)
    if (Baseload_EV < 0)
        Baseload_EV = 0;
    Baseload = MainsMeter.Imeasured - TotalCurrent;                             // Calculate Baseload (load without any active EVSE)

    // ############### now calculate IsetBalanced #################

    if (Mode == MODE_NORMAL)                                                    // Normal Mode
    {
        if (LoadBl == 1)                                                        // Load Balancing = Master? MaxCircuit is max current for all active EVSE's;
            IsetBalanced = (MaxCircuit * 10 ) - Baseload_EV;
                                                                                // limiting is per phase so no Nr_Of_Phases_Charging here!
        else
            IsetBalanced = ChargeCurrent;                                       // No Load Balancing in Normal Mode. Set current to ChargeCurrent (fix: v2.05)
    } //end MODE_NORMAL
    else { // start MODE_SOLAR || MODE_SMART
        // adapt IsetBalanced in Smart Mode, and ensure the MaxMains/MaxCircuit settings for Solar

        uint8_t Temp_Phases;
        Temp_Phases = (Nr_Of_Phases_Charging ? Nr_Of_Phases_Charging : 3);      // in case nr of phases not detected, assume 3
        if ((LoadBl == 0 && EVMeter.Type) || LoadBl == 1)                       // Conditions in which MaxCircuit has to be considered;
                                                                                // mode = Smart/Solar so don't test for that
            Idifference = min((MaxMains * 10) - MainsMeter.Imeasured, (MaxCircuit * 10) - EVMeter.Imeasured);
        else
            Idifference = (MaxMains * 10) - MainsMeter.Imeasured;
        if (MaxSumMains && (Idifference > ((MaxSumMains * 10) - Isum)/Temp_Phases)) {
            Idifference = ((MaxSumMains * 10) - Isum)/Temp_Phases;
            LimitedByMaxSumMains = true;
            _LOG_V("Current is limited by MaxSumMains: MaxSumMains=%iA, Isum=%.1fA, Temp_Phases=%i.\n", MaxSumMains, (float)Isum/10, Temp_Phases);
        }

        if (!mod) {                                                             // no new EVSE's charging
                                                                                // For Smart mode, no new EVSE asking for current
            if (phasesLastUpdateFlag) {                                         // only increase or decrease current if measurements are updated
                _LOG_V("phaseLastUpdate=%i.\n", phasesLastUpdate);
                if (Idifference > 0) {
                    if (Mode == MODE_SMART) IsetBalanced += (Idifference / 4);  // increase with 1/4th of difference (slowly increase current)
                }                                                               // in Solar mode we compute increase of current later on!
                else
                    IsetBalanced += Idifference;                                // last PWM setting + difference (immediately decrease current) (Smart and Solar mode)
            }

            if (IsetBalanced < 0) IsetBalanced = 0;
            if (IsetBalanced > 800) IsetBalanced = 800;                         // hard limit 80A (added 11-11-2017)
        }
        _LOG_V("Checkpoint 2 Isetbalanced=%.1f A, Idifference=%.1f, mod=%i.\n", (float)IsetBalanced/10, (float)Idifference/10, mod);

        if (Mode == MODE_SOLAR)                                                 // Solar version
        {
            IsumImport = Isum - (10 * ImportCurrent);                           // Allow Import of power from the grid when solar charging
            if (Idifference > 0) {                                              // so we had some room for power as far as MaxCircuit and MaxMains are concerned
                if (phasesLastUpdateFlag) {                                     // only increase or decrease current if measurements are updated.
                    if (IsumImport < 0) {
                        // negative, we have surplus (solar) power available
                        if (IsumImport < -10 && Idifference > 10)
                            IsetBalanced = IsetBalanced + 5;                        // more then 1A available, increase Balanced charge current with 0.5A
                        else
                            IsetBalanced = IsetBalanced + 1;                        // less then 1A available, increase with 0.1A
                    } else {
                        // positive, we use more power then is generated
                        if (IsumImport > 20)
                            IsetBalanced = IsetBalanced - (IsumImport / 2);         // we use atleast 2A more then available, decrease Balanced charge current.
                        else if (IsumImport > 10)
                            IsetBalanced = IsetBalanced - 5;                        // we use 1A more then available, decrease with 0.5A
                        else if (IsumImport > 3)
                            IsetBalanced = IsetBalanced - 1;                        // we still use > 0.3A more then available, decrease with 0.1A
                                                                                    // if we use <= 0.3A we do nothing
                    }
                }
            }                                                                   // we already corrected Isetbalance in case of NOT enough power MaxCircuit/MaxMains
            _LOG_V("Checkpoint 3 Isetbalanced=%.1f A, IsumImport=%.1f, Isum=%.1f, ImportCurrent=%i.\n", (float)IsetBalanced/10, (float)IsumImport/10, (float)Isum/10, ImportCurrent);
        } //end MODE_SOLAR
        else { // MODE_SMART
        // New EVSE charging, and only if we have active EVSE's
            if (mod && ActiveEVSE) {                                            // if we have an ActiveEVSE and mod=1, we must be Master, so MaxCircuit has to be
                                                                                // taken into account

                IsetBalanced = min((MaxMains * 10) - Baseload, (MaxCircuit * 10 ) - Baseload_EV ); //assume the current should be available on all 3 phases
                if (MaxSumMains)
                    IsetBalanced = min((int) IsetBalanced, ((MaxSumMains * 10) - Isum)/3); //assume the current should be available on all 3 phases
            }
        } //end MODE_SMART
    } // end MODE_SOLAR || MODE_SMART

    // ############### make sure the calculated IsetBalanced doesnt exceed any boundaries #################

    // Note: all boundary rules must be duplicated to check for HARD shortage of power
    // HARD shortage of power: boundaries are exceeded, we must stop charging!
    // SOFT shortage of power: we have timers running to stop charging in the future
    // guard MaxMains
    if (MainsMeter.Type && Mode != MODE_NORMAL)
        IsetBalanced = min((int) IsetBalanced, (MaxMains * 10) - Baseload); //limiting is per phase so no Nr_Of_Phases_Charging here!
    // guard MaxCircuit
    if ((LoadBl == 0 && EVMeter.Type && Mode != MODE_NORMAL) || LoadBl == 1)    // Conditions in which MaxCircuit has to be considered
        IsetBalanced = min((int) IsetBalanced, (MaxCircuit * 10) - Baseload_EV); //limiting is per phase so no Nr_Of_Phases_Charging here!
    // guard GridRelay
    if (GridRelayOpen) {
        IsetBalanced = min((int) IsetBalanced, (GridRelayMaxSumMains * 10)/Set_Nr_of_Phases_Charging()); //assume the current should be available on all 3 phases
    }
    _LOG_V("Checkpoint 4 Isetbalanced=%.1f A.\n", (float)IsetBalanced/10);

    // ############### the rest of the work we only do if there are ActiveEVSEs #################

    int saveActiveEVSE = ActiveEVSE;                                            // TODO remove this when calcbalancedcurrent2 is approved
    if (ActiveEVSE && (phasesLastUpdateFlag || Mode == MODE_NORMAL)) {          // Only if we have active EVSE's and if we have new phase currents

        // ############### we now check shortage of power  #################

        if (IsetBalanced < (ActiveEVSE * MinCurrent * 10)) {

            // ############### shortage of power  #################

            IsetBalanced = ActiveEVSE * MinCurrent * 10;                        // retain old software behaviour: set minimal "MinCurrent" charge per active EVSE
            if (Mode == MODE_SOLAR) {
                // ----------- Check to see if we have to continue charging on solar power alone ----------
                                              // Importing too much?
                if (ActiveEVSE && StopTime && IsumImport > 0 &&
                        // Would a stop free so much current that StartCurrent would immediately restart charging?
                        Isum > (ActiveEVSE * MinCurrent * Set_Nr_of_Phases_Charging() - StartCurrent) * 10) {
                    //TODO maybe enable solar switching for loadbl = 1
                    if (EnableC2 == AUTO && LoadBl == 0)
                        Set_Nr_of_Phases_Charging();
                    if (Nr_Of_Phases_Charging > 1 && EnableC2 == AUTO && LoadBl == 0) { // when loadbalancing is enabled we don't do forced single phase charging
                        _LOG_A("Switching to single phase.\n");                 // because we wouldnt know which currents to make available to the nodes...
                                                                                // since we don't know how many phases the nodes are using...
                        //switching contactor2 off works ok for Skoda Enyaq but Hyundai Ioniq 5 goes into error, so we have to switch more elegantly
                        if (State == STATE_C) setState(STATE_C1);               // tell EV to stop charging
                        Switching_To_Single_Phase = GOING_TO_SWITCH;
                    }
                    else {
                        if (SolarStopTimer == 0) setSolarStopTimer(StopTime * 60); // Convert minutes into seconds
                    }
                } else {
                    _LOG_D("Checkpoint a: Resetting SolarStopTimer, IsetBalanced=%.1fA, ActiveEVSE=%i.\n", (float)IsetBalanced/10, ActiveEVSE);
                    setSolarStopTimer(0);
                }
            }

            // check for HARD shortage of power
            // with HARD shortage we stop charging
            // with SOFT shortage we have a timer running
            // IsetBalanced is already set to the minimum needed power to charge all Nodes
            bool hardShortage = false;
            // guard MaxMains
            if (MainsMeter.Type && Mode != MODE_NORMAL)
                if (IsetBalanced > (MaxMains * 10) - Baseload)
                    hardShortage = true;
            // guard MaxCircuit
            if (((LoadBl == 0 && EVMeter.Type && Mode != MODE_NORMAL) || LoadBl == 1) // Conditions in which MaxCircuit has to be considered
                && (IsetBalanced > (MaxCircuit * 10) - Baseload_EV))
                    hardShortage = true;
            if (!MaxSumMainsTime && LimitedByMaxSumMains)                       // if we don't use the Capacity timer, we want a hard stop
                hardShortage = true;
            if (hardShortage && Switching_To_Single_Phase != GOING_TO_SWITCH) { // because switching to single phase might solve the shortage
                // ############ HARD shortage of power
                NoCurrent++;                                                    // Flag NoCurrent left
                _LOG_I("No Current!!\n");
            } else {
                // ############ soft shortage of power
                // the expiring of both SolarStopTimer and MaxSumMainsTimer is handled in the Timer1s loop
                if (LimitedByMaxSumMains && MaxSumMainsTime) {
                    if (MaxSumMainsTimer == 0)                                  // has expired, so set timer
                        MaxSumMainsTimer = MaxSumMainsTime * 60;
                }
            }
        } else {                                                                // we have enough current
            // ############### no shortage of power  #################

            _LOG_D("Checkpoint b: Resetting SolarStopTimer, MaxSumMainsTimer, IsetBalanced=%.1fA, ActiveEVSE=%i.\n", (float)IsetBalanced/10, ActiveEVSE);
            setSolarStopTimer(0);
            MaxSumMainsTimer = 0;
            NoCurrent = 0;
        }

        // ############### we now distribute the calculated IsetBalanced over the EVSEs  #################

        if (IsetBalanced > ActiveMax) IsetBalanced = ActiveMax;                 // limit to total maximum Amps (of all active EVSE's)
                                                                                // TODO not sure if Nr_Of_Phases_Charging should be involved here
        MaxBalanced = IsetBalanced;                                             // convert to Amps

        // Calculate average current per EVSE
        n = 0;
        while (n < NR_EVSES && ActiveEVSE) {
            Average = MaxBalanced / ActiveEVSE;                                 // Average current for all active EVSE's

            // Active EVSE, and current not yet calculated?
            if ((BalancedState[n] == STATE_C) && (!CurrentSet[n])) {            

                // Check for EVSE's that are starting with Solar charging
                if ((Mode == MODE_SOLAR) && (Node[n].IntTimer < SOLARSTARTTIME)) {
                    Balanced[n] = MinCurrent * 10;                              // Set to MinCurrent
                    _LOG_V("[S]Node %u = %u.%u A", n, Balanced[n]/10, Balanced[n]%10);
                    CurrentSet[n] = 1;                                          // mark this EVSE as set.
                    ActiveEVSE--;                                               // decrease counter of active EVSE's
                    MaxBalanced -= Balanced[n];                                 // Update total current to new (lower) value
                    IsetBalanced = TotalCurrent;
                    n = 0;                                                      // reset to recheck all EVSE's
                    continue;                                                   // ensure the loop restarts from the beginning
                
                // Check for EVSE's that have a Max Current that is lower then the average
                } else if (Average >= BalancedMax[n]) {
                    Balanced[n] = BalancedMax[n];                               // Set current to Maximum allowed for this EVSE
                    _LOG_V("[L]Node %u = %u.%u A", n, Balanced[n]/10, Balanced[n]%10);
                    CurrentSet[n] = 1;                                          // mark this EVSE as set.
                    ActiveEVSE--;                                               // decrease counter of active EVSE's
                    MaxBalanced -= Balanced[n];                                 // Update total current to new (lower) value
                    n = 0;                                                      // reset to recheck all EVSE's
                    continue;                                                   // ensure the loop restarts from the beginning
                }

            }
            n++;
        }

        // All EVSE's which had a Max current lower then the average are set.
        // Now calculate the current for the EVSE's which had a higher Max current
        n = 0;
        while (n < NR_EVSES && ActiveEVSE) {                                    // Check for EVSE's that are not set yet
            if ((BalancedState[n] == STATE_C) && (!CurrentSet[n])) {            // Active EVSE, and current not yet calculated?
                Balanced[n] = MaxBalanced / ActiveEVSE;                         // Set current to Average
                _LOG_V("[H]Node %u = %u.%u A.\n", n, Balanced[n]/10, Balanced[n]%10);
                CurrentSet[n] = 1;                                              // mark this EVSE as set.
                ActiveEVSE--;                                                   // decrease counter of active EVSE's
                MaxBalanced -= Balanced[n];                                     // Update total current to new (lower) value
            }                                                                   //TODO since the average has risen the other EVSE's should be checked for exceeding their MAX's too!
            n++;
        }
    } //ActiveEVSE && phasesLastUpdateFlag

    if (!saveActiveEVSE) { // no ActiveEVSEs so reset all timers
        _LOG_D("Checkpoint c: Resetting SolarStopTimer, MaxSumMainsTimer, IsetBalanced=%.1fA, saveActiveEVSE=%i.\n", (float)IsetBalanced/10, saveActiveEVSE);
        setSolarStopTimer(0);
        MaxSumMainsTimer = 0;
        NoCurrent = 0;
    }

    // Reset flag that keeps track of new MainsMeter measurements
    phasesLastUpdateFlag = false;

    // ############### print all the distributed currents #################

    _LOG_V("Checkpoint 5 Isetbalanced=%.1f A.\n", (float)IsetBalanced/10);
    if (LoadBl == 1) {
        _LOG_D("Balance: ");
        for (n = 0; n < NR_EVSES; n++) {
            _LOG_D_NO_FUNC("EVSE%u:%s(%.1fA) ", n, getStateName(BalancedState[n]), (float)Balanced[n]/10);
        }
        _LOG_D_NO_FUNC("\n");
    }
} //CalcBalancedCurrent

/**
 * Load Balancing 	Modbus Address  LoadBl
    Disabled     	0x01            0x00
    Master       	0x01            0x01
    Node 1 	        0x02            0x02
    Node 2 	        0x03            0x03
    Node 3 	        0x04            0x04
    Node 4 	        0x05            0x05
    Node 5 	        0x06            0x06
    Node 6 	        0x07            0x07
    Node 7 	        0x08            0x08
    Broadcast to all SmartEVSE with address 0x09.
**/

/**
 * In order to keep each node happy, and not timeout with a comm-error you will have to send the chargecurrent for each node in a broadcast message to all nodes
 * (address 09):

    09 10 00 20 00 08 10 00 A0 00 00 00 3C 00 00 00 00 00 00 00 00 00 00 99 24
    Node 0 00 A0 = 160 = 16.0A
    Node 1 00 00 = 0 = 0.0A
    Node 2 00 3C = 60 = 6.0A
    etc.

 *  Each time this message is received on each node, the timeout timer is reset to 10 seconds.
 *  The master will usually send this message every two seconds.
**/

/**
 * Broadcast momentary currents to all Node EVSE's
 */
void BroadcastCurrent(void) {
    //prepare registers 0x0020 thru 0x002A (including) to be sent
    uint8_t buf[sizeof(Balanced)+ 6], i;
    uint8_t *p=buf;
    memcpy(p, Balanced, sizeof(Balanced));
    p = p + sizeof(Balanced);
    // Irms values, we only send the 16 least significant bits (range -327.6A to +327.6A) per phase
    for ( i=0; i<3; i++) {
        p[i * 2] = MainsMeter.Irms[i] & 0xff;
        p[(i * 2) + 1] = MainsMeter.Irms[i] >> 8;
    }
    ModbusWriteMultipleRequest(BROADCAST_ADR, 0x0020, (uint16_t *) buf, 8 + 3);
}

/**
 * EVSE Register 0x02*: System configuration (same on all SmartEVSE in a LoadBalancing setup)
 * TODO not sure if this is used anywhere in the code?
Regis 	Access 	Description 	                                        Unit 	Values
0x0200 	R/W 	EVSE mode 		                                        0:Normal / 1:Smart / 2:Solar
0x0201 	R/W 	EVSE Circuit max Current 	                        A 	10 - 160
0x0202 	R/W 	Grid type to which the Sensorbox is connected 		        0:4Wire / 1:3Wire
0x0203 	R/W 	Sensorbox 2 WiFi Mode                                   0:Disabled / 1:Enabled / 2:Portal
0x0204 	R/W 	Max Mains Current 	                                A 	10 - 200
0x0205 	R/W 	Surplus energy start Current 	                        A 	1 - 16
0x0206 	R/W 	Stop solar charging at 6A after this time 	        min 	0:Disable / 1 - 60
0x0207 	R/W 	Allow grid power when solar charging 	                A 	0 - 6
0x0208 	R/W 	Type of Mains electric meter 		                *
0x0209 	R/W 	Address of Mains electric meter 		                10 - 247
//0x020A 	R/W 	What does Mains electric meter measure 		                0:Mains (Home+EVSE+PV) / 1:Home+EVSE
0x020B 	R/W 	Type of PV electric meter 		                *
0x020C 	R/W 	Address of PV electric meter 		                        10 - 247
0x020D 	R/W 	Byte order of custom electric meter 		                0:LBF & LWF / 1:LBF & HWF / 2:HBF & LWF / 3:HBF & HWF
0x020E 	R/W 	Data type of custom electric meter 		                0:Integer / 1:Double
0x020F 	R/W 	Modbus Function (3/4) of custom electric meter
0x0210 	R/W 	Register for Voltage (V) of custom electric meter 		0 - 65530
0x0211 	R/W 	Divisor for Voltage (V) of custom electric meter 	10x 	0 - 7
0x0212 	R/W 	Register for Current (A) of custom electric meter 		0 - 65530
0x0213 	R/W 	Divisor for Current (A) of custom electric meter 	10x 	0 - 7
0x0214 	R/W 	Register for Power (W) of custom electric meter 		0 - 65534
0x0215 	R/W 	Divisor for Power (W) of custom electric meter 	        10x 	0 - 7 /
0x0216 	R/W 	Register for Energy (kWh) of custom electric meter 		0 - 65534
0x0217 	R/W 	Divisor for Energy (kWh) of custom electric meter 	10x 	0 - 7
0x0218 	R/W 	Maximum register read (Not implemented)
0x0219 	R/W 	WiFi mode
0x021A 	R/W 	Limit max current draw on MAINS (sum of phases) 	A 	9:Disable / 10 - 200
**/

/**
 * Master requests Node configuration over modbus
 * Master -> Node
 * 
 * @param uint8_t NodeNr (1-7)
 */
void requestNodeConfig(uint8_t NodeNr) {
    ModbusReadInputRequest(NodeNr + 1u, 4, 0x0108, 2);
}

/**
 * EVSE Node Config layout
 *
Reg 	Access 	Description 	                        Unit 	Values
0x0100 	R/W 	Configuration 		                        0:Socket / 1:Fixed Cable
0x0101 	R/W 	Cable lock 		                        0:Disable / 1:Solenoid / 2:Motor
0x0102 	R/W 	MIN Charge Current the EV will accept 	A 	6 - 16
0x0103 	R/W 	MAX Charge Current for this EVSE 	A 	6 - 80
0x0104 	R/W 	Load Balance 		                        0:Disabled / 1:Master / 2-8:Node
0x0105 	R/W 	External Switch on pin SW 		        0:Disabled / 1:Access Push-Button / 2:Access Switch / 3:Smart-Solar Push-Button / 4:Smart-Solar Switch
0x0106 	R/W 	Residual Current Monitor on pin RCM 		0:Disabled / 1:Enabled
0x0107 	R/W 	Use RFID reader 		                0:Disabled / 1:Enabled
0x0108 	R/W 	Type of EV electric meter 		        *
0x0109 	R/W 	Address of EV electric meter 		        10 - 247
**/

/**
 * Master receives Node configuration over modbus
 * Node -> Master
 * 
 * @param uint8_t NodeNr (1-7)
 */
void receiveNodeConfig(uint8_t *buf, uint8_t NodeNr) {
    Node[NodeNr].EVMeter = buf[1];
    Node[NodeNr].EVAddress = buf[3];

    Node[NodeNr].ConfigChanged = 0;                                             // Reset flag on master
    ModbusWriteSingleRequest(NodeNr + 1u, 0x0006, 0);                           // Reset flag on node
}

/**
 * Master requests Node status over modbus
 * Master -> Node
 *
 * @param uint8_t NodeNr (1-7)
 */
void requestNodeStatus(uint8_t NodeNr) {
    if(Node[NodeNr].Online) {
        if(Node[NodeNr].Online-- == 1) {
            // Reset Node state when node is offline
            BalancedState[NodeNr] = STATE_A;
            Balanced[NodeNr] = 0;
        }
    }

    ModbusReadInputRequest(NodeNr + 1u, 4, 0x0000, 8);
}

/** To have full control over the nodes, you will have to read each node's status registers, and see if it requests to charge.
 * for example for node 2:

    Received packet (21 bytes) 03 04 10 00 01 00 00 00 3c 00 01 00 00 00 01 00 01 00 20 4d 8c
    00 01 = state B
    00 00 = no errors
    00 3c = charge current 6.0 A
    00 01 = Smart mode
    etc.

    Here the state changes to STATE_COMM_C (00 06)
    Received packet (21 bytes) 03 04 10 00 06 00 00 00 3c 00 01 00 00 00 01 00 01 00 20 0a 8e
    So the ESVE request to charge.

    You can respond to this request by changing the state of the node to State_C
    03 10 00 00 00 02 04 00 07 00 00 49 D6
    Here it will write 00 07 (STATE_COMM_C_OK) to register 0x0000, and reset the error register 0x0001

    The node will respond to this by switching to STATE_C (Charging).
**/

/**
 * EVSE Node status layout
 *
Regist 	Access  Description 	        Unit 	Values
0x0000 	R/W 	State 		                0:A / 1:B / 2:C / 3:D / 4:Node request B / 5:Master confirm B / 6:Node request C /
                                                7:Master confirm C / 8:Activation mode / 9:B1 / 10:C1
0x0001 	R/W 	Error 	                Bit 	1:LESS_6A / 2:NO_COMM / 4:TEMP_HIGH / 8:EV_NOCOMM / 16:RCD / 32:NO_SUN
0x0002 	R/W 	Charging current        0.1 A 	0:no current available / 6-80
0x0003 	R/W 	EVSE mode (without saving)      0:Normal / 1:Smart / 2:Solar
0x0004 	R/W 	Solar Timer 	        s
0x0005 	R/W 	Access bit 		        0:No Access / 1:Access
0x0006 	R/W 	Configuration changed (Not implemented)
0x0007 	R 	Maximum charging current A
0x0008 	R/W 	Number of used phases (Not implemented) 0:Undetected / 1 - 3
0x0009 	R 	Real charging current (Not implemented) 0.1 A
0x000A 	R 	Temperature 	        K
0x000B 	R 	Serial number
0x0020 - 0x0027
        W 	Broadcast charge current. SmartEVSE uses only one value depending on the "Load Balancing" configuration
                                        0.1 A 	0:no current available
0x0028 - 0x0030
        W 	Broadcast MainsMeter currents L1 - L3.
                                        0.1 A
**/

/**
 * Master receives Node status over modbus
 * Node -> Master
 *
 * @param uint8_t NodeAdr (1-7)
 */
void receiveNodeStatus(uint8_t *buf, uint8_t NodeNr) {
    Node[NodeNr].Online = 5;

    BalancedState[NodeNr] = buf[1];                                             // Node State
    BalancedError[NodeNr] = buf[3];                                             // Node Error status
    // Update Mode when changed on Node and not Smart/Solar Switch on the Master
    // Also make sure we are not in the menu.
    Node[NodeNr].Mode = buf[7];

    if ((Node[NodeNr].Mode != Mode) && Switch != 4 && !LCDNav && !NodeNewMode) {
        NodeNewMode = Node[NodeNr].Mode + 1;        // Store the new Mode in NodeNewMode, we'll update Mode in 'ProcessAllNodeStates'
    }
    Node[NodeNr].SolarTimer = (buf[8] * 256) + buf[9];
    Node[NodeNr].ConfigChanged = buf[13] | Node[NodeNr].ConfigChanged;
    BalancedMax[NodeNr] = buf[15] * 10;                                         // Node Max ChargeCurrent (0.1A)
    _LOG_D("ReceivedNode[%u]Status State:%u (%s) Error:%u, BalancedMax:%u, Mode:%u, ConfigChanged:%u.\n", NodeNr, BalancedState[NodeNr], StrStateName[BalancedState[NodeNr]], BalancedError[NodeNr], BalancedMax[NodeNr], Node[NodeNr].Mode, Node[NodeNr].ConfigChanged);
}

/**
 * Master checks node status requests, and responds with new state
 * Master -> Node
 *
 * @param uint8_t NodeAdr (1-7)
 * @return uint8_t success
 */
uint8_t processAllNodeStates(uint8_t NodeNr) {
    uint16_t values[5];
    uint8_t current, write = 0, regs = 2;                                       // registers are written when Node needs updating.

    values[0] = BalancedState[NodeNr];

    current = IsCurrentAvailable();
    if (current) {                                                              // Yes enough current
        if (BalancedError[NodeNr] & (LESS_6A|NO_SUN)) {
            BalancedError[NodeNr] &= ~(LESS_6A | NO_SUN);                       // Clear Error flags
            write = 1;
        }
    }

    if ((ErrorFlags & CT_NOCOMM) && !(BalancedError[NodeNr] & CT_NOCOMM)) {
        BalancedError[NodeNr] |= CT_NOCOMM;                                     // Send Comm Error on Master to Node
        write = 1;
    }

    // Check EVSE for request to charge states
    switch (BalancedState[NodeNr]) {
        case STATE_A:
            // Reset Node
            Node[NodeNr].IntTimer = 0;
            Node[NodeNr].Timer = 0;
            Node[NodeNr].Phases = 0;
            Node[NodeNr].MinCurrent = 0;
            break;

        case STATE_COMM_B:                                                      // Request to charge A->B
            _LOG_I("Node %u State A->B request ", NodeNr);
            if (current) {                                                      // check if we have enough current
                                                                                // Yes enough current..
                BalancedState[NodeNr] = STATE_B;                                // Mark Node EVSE as active (State B)
                Balanced[NodeNr] = MinCurrent * 10;                             // Initially set current to lowest setting
                values[0] = STATE_COMM_B_OK;
                write = 1;
                _LOG_I("- OK!\n");
            } else {                                                            // We do not have enough current to start charging
                Balanced[NodeNr] = 0;                                           // Make sure the Node does not start charging by setting current to 0
                if ((BalancedError[NodeNr] & (LESS_6A|NO_SUN)) == 0) {          // Error flags cleared?
                    if (Mode == MODE_SOLAR) BalancedError[NodeNr] |= NO_SUN;    // Solar mode: No Solar Power available
                    else BalancedError[NodeNr] |= LESS_6A;                      // Normal or Smart Mode: Not enough current available
                    write = 1;
                }
                _LOG_I("- Not enough current!\n");
            }
            break;

        case STATE_COMM_C:                                                      // request to charge B->C
            _LOG_I("Node %u State B->C request\n", NodeNr);
            Balanced[NodeNr] = 0;                                               // For correct baseload calculation set current to zero
            if (current) {                                                      // check if we have enough current
                                                                                // Yes
                BalancedState[NodeNr] = STATE_C;                                // Mark Node EVSE as Charging (State C)
                CalcBalancedCurrent(1);                                         // Calculate charge current for all connected EVSE's
                values[0] = STATE_COMM_C_OK;
                write = 1;
                _LOG_I("- OK!\n");
            } else {                                                            // We do not have enough current to start charging
                if ((BalancedError[NodeNr] & (LESS_6A|NO_SUN)) == 0) {          // Error flags cleared?
                    if (Mode == MODE_SOLAR) BalancedError[NodeNr] |= NO_SUN;    // Solar mode: No Solar Power available
                    else BalancedError[NodeNr] |= LESS_6A;                      // Normal or Smart Mode: Not enough current available
                    write = 1;
                }
                _LOG_I("- Not enough current!\n");
            }
            break;

        default:
            break;

    }

    // Here we set the Masters Mode to the one we received from a Slave/Node
    if (NodeNewMode) {
        setMode(NodeNewMode -1);
        NodeNewMode = 0;
    }    

    // Error Flags
    values[1] = BalancedError[NodeNr];
    // Charge Current
    values[2] = 0;                                                              // This does nothing for Nodes. Currently the Chargecurrent can only be written to the Master
    // Mode
    if (Node[NodeNr].Mode != Mode) {
        regs = 4;
        write = 1;
    }    
    values[3] = Mode;
    
    // SolarStopTimer
    if (abs((int16_t)SolarStopTimer - (int16_t)Node[NodeNr].SolarTimer) > 3) {  // Write SolarStoptimer to Node if time is off by 3 seconds or more.
        regs = 5;
        write = 1;
        values[4] = SolarStopTimer;
    }    

    if (write) {
        _LOG_D("processAllNode[%u]States State:%u (%s), BalancedError:%u, Mode:%u, SolarStopTimer:%u\n",NodeNr, BalancedState[NodeNr], StrStateName[BalancedState[NodeNr]], BalancedError[NodeNr], Mode, SolarStopTimer);
        ModbusWriteMultipleRequest(NodeNr+1 , 0x0000, values, regs);            // Write State, Error, Charge Current, Mode and Solar Timer to Node
    }

    return write;
}


/**
 * Check minimum and maximum of a value and set the variable
 *
 * @param uint8_t MENU_xxx
 * @param uint16_t value
 * @return uint8_t success
 */
uint8_t setItemValue(uint8_t nav, uint16_t val) {
    if (nav < MENU_EXIT) {
        if (val < MenuStr[nav].Min || val > MenuStr[nav].Max) return 0;
    }

    switch (nav) {
        case MENU_MAX_TEMP:
            maxTemp = val;
            break;
        case MENU_C2:
            EnableC2 = (EnableC2_t) val;
            break;
        case MENU_CONFIG:
            Config = val;
            break;
        case STATUS_MODE:
            if (Mode != val)                                                    // this prevents slave from waking up from OFF mode when Masters'
                                                                                // solarstoptimer starts to count
                setMode(val);
            break;
        case MENU_MODE:
            Mode = val;
            break;
        case MENU_START:
            StartCurrent = val;
            break;
        case MENU_STOP:
            StopTime = val;
            break;
        case MENU_IMPORT:
            ImportCurrent = val;
            break;
        case MENU_LOADBL:
#if SMARTEVSE_VERSION == 3
            ConfigureModbusMode(val);
#endif
            LoadBl = val;
            break;
        case MENU_MAINS:
            MaxMains = val;
            break;
        case MENU_SUMMAINS:
            MaxSumMains = val;
            break;
        case MENU_SUMMAINSTIME:
            MaxSumMainsTime = val;
            break;
        case MENU_MIN:
            MinCurrent = val;
            break;
        case MENU_MAX:
            MaxCurrent = val;
            break;
        case MENU_CIRCUIT:
            MaxCircuit = val;
            break;
        case MENU_LOCK:
            Lock = val;
            break;
        case MENU_SWITCH:
            Switch = val;
            break;
        case MENU_RCMON:
            RCmon = val;
            break;
        case MENU_GRID:
            Grid = val;
            break;
        case MENU_SB2_WIFI:
            SB2_WIFImode = val;
            break;    
        case MENU_MAINSMETER:
            MainsMeter.Type = val;
            break;
        case MENU_MAINSMETERADDRESS:
            MainsMeter.Address = val;
            break;
        case MENU_EVMETER:
            EVMeter.Type = val;
            break;
        case MENU_EVMETERADDRESS:
            EVMeter.Address = val;
            break;
        case MENU_EMCUSTOM_ENDIANESS:
            EMConfig[EM_CUSTOM].Endianness = val;
            break;
        case MENU_EMCUSTOM_DATATYPE:
            EMConfig[EM_CUSTOM].DataType = (mb_datatype)val;
            break;
        case MENU_EMCUSTOM_FUNCTION:
            EMConfig[EM_CUSTOM].Function = val;
            break;
        case MENU_EMCUSTOM_UREGISTER:
            EMConfig[EM_CUSTOM].URegister = val;
            break;
        case MENU_EMCUSTOM_UDIVISOR:
            EMConfig[EM_CUSTOM].UDivisor = val;
            break;
        case MENU_EMCUSTOM_IREGISTER:
            EMConfig[EM_CUSTOM].IRegister = val;
            break;
        case MENU_EMCUSTOM_IDIVISOR:
            EMConfig[EM_CUSTOM].IDivisor = val;
            break;
        case MENU_EMCUSTOM_PREGISTER:
            EMConfig[EM_CUSTOM].PRegister = val;
            break;
        case MENU_EMCUSTOM_PDIVISOR:
            EMConfig[EM_CUSTOM].PDivisor = val;
            break;
        case MENU_EMCUSTOM_EREGISTER:
            EMConfig[EM_CUSTOM].ERegister = val;
            break;
        case MENU_EMCUSTOM_EDIVISOR:
            EMConfig[EM_CUSTOM].EDivisor = val;
            break;
        case MENU_RFIDREADER:
            RFIDReader = val;
            break;
        case MENU_WIFI:
            WIFImode = val;
            break;    
        case MENU_AUTOUPDATE:
            AutoUpdate = val;
            break;

        // Status writeable
        case STATUS_STATE:
            if (val != State) setState(val);
            break;
        case STATUS_ERROR:
            ErrorFlags = val;
            if (ErrorFlags) {                                                   // Is there an actual Error? Maybe the error got cleared?
                if (ErrorFlags & CT_NOCOMM) MainsMeter.Timeout = 0;             // clear MainsMeter.Timeout on a CT_NOCOMM error, so the error will be immediate.
                setStatePowerUnavailable();
                ChargeDelay = CHARGEDELAY;
                _LOG_V("Error message received!\n");
            } else {
                _LOG_V("Errors Cleared received!\n");
            }
            break;
        case STATUS_CURRENT:
            OverrideCurrent = val;
            if (LoadBl < 2) MainsMeter.Timeout = COMM_TIMEOUT;                  // reset timeout when register is written
            break;
        case STATUS_SOLAR_TIMER:
            SolarStopTimer = val;
            break;
        case STATUS_ACCESS:
            if (val == 0 || val == 1) {
                setAccess(val);
            }
            break;
        case STATUS_CONFIG_CHANGED:
            ConfigChanged = val;
            break;

        default:
            return 0;
    }

    return 1;
}

/**
 * Get the variable
 *
 * @param uint8_t MENU_xxx
 * @return uint16_t value
 */
uint16_t getItemValue(uint8_t nav) {
    switch (nav) {
        case MENU_MAX_TEMP:
            return maxTemp;
        case MENU_C2:
            return EnableC2;
        case MENU_CONFIG:
            return Config;
        case MENU_MODE:
        case STATUS_MODE:
            return Mode;
        case MENU_START:
            return StartCurrent;
        case MENU_STOP:
            return StopTime;
        case MENU_IMPORT:
            return ImportCurrent;
        case MENU_LOADBL:
            return LoadBl;
        case MENU_MAINS:
            return MaxMains;
        case MENU_SUMMAINS:
            return MaxSumMains;
        case MENU_SUMMAINSTIME:
            return MaxSumMainsTime;
        case MENU_MIN:
            return MinCurrent;
        case MENU_MAX:
            return MaxCurrent;
        case MENU_CIRCUIT:
            return MaxCircuit;
        case MENU_LOCK:
            return Lock;
        case MENU_SWITCH:
            return Switch;
        case MENU_RCMON:
            return RCmon;
        case MENU_GRID:
            return Grid;
        case MENU_SB2_WIFI:
            return SB2_WIFImode;
        case MENU_MAINSMETER:
            return MainsMeter.Type;
        case MENU_MAINSMETERADDRESS:
            return MainsMeter.Address;
        case MENU_EVMETER:
            return EVMeter.Type;
        case MENU_EVMETERADDRESS:
            return EVMeter.Address;
        case MENU_EMCUSTOM_ENDIANESS:
            return EMConfig[EM_CUSTOM].Endianness;
        case MENU_EMCUSTOM_DATATYPE:
            return EMConfig[EM_CUSTOM].DataType;
        case MENU_EMCUSTOM_FUNCTION:
            return EMConfig[EM_CUSTOM].Function;
        case MENU_EMCUSTOM_UREGISTER:
            return EMConfig[EM_CUSTOM].URegister;
        case MENU_EMCUSTOM_UDIVISOR:
            return EMConfig[EM_CUSTOM].UDivisor;
        case MENU_EMCUSTOM_IREGISTER:
            return EMConfig[EM_CUSTOM].IRegister;
        case MENU_EMCUSTOM_IDIVISOR:
            return EMConfig[EM_CUSTOM].IDivisor;
        case MENU_EMCUSTOM_PREGISTER:
            return EMConfig[EM_CUSTOM].PRegister;
        case MENU_EMCUSTOM_PDIVISOR:
            return EMConfig[EM_CUSTOM].PDivisor;
        case MENU_EMCUSTOM_EREGISTER:
            return EMConfig[EM_CUSTOM].ERegister;
        case MENU_EMCUSTOM_EDIVISOR:
            return EMConfig[EM_CUSTOM].EDivisor;
        case MENU_RFIDREADER:
            return RFIDReader;
        case MENU_WIFI:
            return WIFImode;    
        case MENU_AUTOUPDATE:
            return AutoUpdate;

        // Status writeable
        case STATUS_STATE:
            return State;
        case STATUS_ERROR:
            return ErrorFlags;
        case STATUS_CURRENT:
            return Balanced[0];
        case STATUS_SOLAR_TIMER:
            return SolarStopTimer;
        case STATUS_ACCESS:
            return Access_bit;
        case STATUS_CONFIG_CHANGED:
            return ConfigChanged;

        // Status readonly
        case STATUS_MAX:
            return min(MaxCapacity,MaxCurrent);
        case STATUS_TEMP:
            return (signed int)TempEVSE;
        case STATUS_SERIAL:
            return serialnr;

        default:
            return 0;
    }
}


void printStatus(void)
{
    _LOG_I ("STATE: %s Error: %u StartCurrent: -%i ChargeDelay: %u SolarStopTimer: %u NoCurrent: %u Imeasured: %.1f A IsetBalanced: %.1f A, MainsMeter.Timeout=%u, EVMeter.Timeout=%u.\n", getStateName(State), ErrorFlags, StartCurrent, ChargeDelay, SolarStopTimer,  NoCurrent, (float)MainsMeter.Imeasured/10, (float)IsetBalanced/10, MainsMeter.Timeout, EVMeter.Timeout);
    _LOG_I("L1: %.1f A L2: %.1f A L3: %.1f A Isum: %.1f A\n", (float)MainsMeter.Irms[0]/10, (float)MainsMeter.Irms[1]/10, (float)MainsMeter.Irms[2]/10, (float)Isum/10);
}

#if MODEM
// Recompute State of Charge, in case we have a known initial state of charge
// This function is called by kWh logic and after an EV state update through API, Serial or MQTT
void RecomputeSoC(void) {
    if (InitialSoC > 0 && FullSoC > 0 && EnergyCapacity > 0) {
        if (InitialSoC == FullSoC) {
            // We're already at full SoC
            ComputedSoC = FullSoC;
            RemainingSoC = 0;
            TimeUntilFull = -1;
        } else {
            int EnergyRemaining = -1;
            int TargetEnergyCapacity = (FullSoC / 100.f) * EnergyCapacity;

            if (EnergyRequest > 0) {
                // Attempt to use EnergyRequest to determine SoC with greater accuracy
                EnergyRemaining = EVMeter.EnergyCharged > 0 ? (EnergyRequest - EVMeter.EnergyCharged) : EnergyRequest;
            } else {
                // We use a rough estimation based on FullSoC and EnergyCapacity
                EnergyRemaining = TargetEnergyCapacity - (EVMeter.EnergyCharged + (InitialSoC / 100.f) * EnergyCapacity);
            }

            RemainingSoC = ((FullSoC * EnergyRemaining) / TargetEnergyCapacity);
            ComputedSoC = RemainingSoC > 1 ? (FullSoC - RemainingSoC) : FullSoC;

            // Only attempt to compute the SoC and TimeUntilFull if we have a EnergyRemaining and PowerMeasured
            if (EnergyRemaining > -1) {
                int TimeToGo = -1;
                // Do a very simple estimation in seconds until car would reach FullSoC according to current charging power
                if (EVMeter.PowerMeasured > 0) {
                    // Use real-time PowerMeasured data if available
                    TimeToGo = (3600 * EnergyRemaining) / EVMeter.PowerMeasured;
                } else if (Nr_Of_Phases_Charging > 0) {
                    // Else, fall back on the theoretical maximum of the cable + nr of phases
                    TimeToGo = (3600 * EnergyRemaining) / (MaxCapacity * (Nr_Of_Phases_Charging * 230));
                }

                // Wait until we have a somewhat sensible estimation while still respecting granny chargers
                if (TimeToGo < 100000) {
                    TimeUntilFull = TimeToGo;
                }
            }

            // We can't possibly charge to over 100% SoC
            if (ComputedSoC > FullSoC) {
                ComputedSoC = FullSoC;
                RemainingSoC = 0;
                TimeUntilFull = -1;
            }

            _LOG_I("SoC: EnergyRemaining %i RemaningSoC %i EnergyRequest %i EnergyCharged %i EnergyCapacity %i ComputedSoC %i FullSoC %i TimeUntilFull %i TargetEnergyCapacity %i\n", EnergyRemaining, RemainingSoC, EnergyRequest, EVMeter.EnergyCharged, EnergyCapacity, ComputedSoC, FullSoC, TimeUntilFull, TargetEnergyCapacity);
        }
    } else {
        if (TimeUntilFull != -1) TimeUntilFull = -1;
    }
    // There's also the possibility an external API/app is used for SoC info. In such case, we allow setting ComputedSoC directly.
}
#endif

// EV disconnected from charger. Triggered after 60 seconds of disconnect
// This is done so we can "re-plug" the car in the Modem process without triggering disconnect events
void DisconnectEvent(void){
    _LOG_A("EV disconnected for a while. Resetting SoC states");
    ModemStage = 0; // Enable Modem states again
    InitialSoC = -1;
    FullSoC = -1;
    RemainingSoC = -1;
    ComputedSoC = -1;
    EnergyCapacity = -1;
    EnergyRequest = -1;
    TimeUntilFull = -1;
    strncpy(EVCCID, "", sizeof(EVCCID));
}

void CalcIsum(void) {
    phasesLastUpdate = time(NULL);
    phasesLastUpdateFlag = true;                        // Set flag if a new Irms measurement is received.
    int batteryPerPhase = getBatteryCurrent() / 3;
    Isum = 0;
#if FAKE_SUNNY_DAY
    int32_t temp[3]={0, 0, 0};
    temp[0] = INJECT_CURRENT_L1 * 10;                   //Irms is in units of 100mA
    temp[1] = INJECT_CURRENT_L2 * 10;
    temp[2] = INJECT_CURRENT_L3 * 10;
#endif

    for (int x = 0; x < 3; x++) {
#if FAKE_SUNNY_DAY
        MainsMeter.Irms[x] = MainsMeter.Irms[x] - temp[x];
#endif
        IrmsOriginal[x] = MainsMeter.Irms[x];
        MainsMeter.Irms[x] -= batteryPerPhase;
        Isum = Isum + MainsMeter.Irms[x];
    }
    MainsMeter.CalcImeasured();
}


// CheckSwitch (SW input)
//
void CheckSwitch(bool force = false)
{
    static uint8_t RB2count = 0, RB2last = 2, RB2low = 0;
    static unsigned long RB2Timer = 0;                                                 // 1500ms

    if (force)                                                                  // force to read switch position
        RB2last = 2;

    if ((RB2last == 2) && (Switch == 1 || Switch == 3))                         // upon initialization we want the toggle switch to be read
        RB2last = 1;                                                            // but not the push buttons, because this would toggle the state
                                                                                // upon reboot
    // External switch changed state?
    if ( (digitalRead(PIN_SW_IN) != RB2last) || RB2low) {
        // make sure that noise on the input does not switch
        if (RB2count++ > 20 || RB2low) {
            RB2last = digitalRead(PIN_SW_IN);
            if (RB2last == 0) {
                // Switch input pulled low
                switch (Switch) {
                    case 1: // Access Button
                        setAccess(!Access_bit);                             // Toggle Access bit on/off
                        _LOG_I("Access: %d\n", Access_bit);
                        break;
                    case 2: // Access Switch
                        setAccess(true);
                        break;
                    case 3: // Smart-Solar Button or hold button for 1,5 second to STOP charging
                        if (RB2low == 0) {
                            RB2low = 1;
                            RB2Timer = millis();
                        }
                        if (RB2low && millis() > RB2Timer + 1500) {
                            if (State == STATE_C) {
                                setState(STATE_C1);
                                if (!TestState) ChargeDelay = 15;           // Keep in State B for 15 seconds, so the Charge cable can be removed.
                            RB2low = 2;
                            }
                        }
                        break;
                    case 4: // Smart-Solar Switch
                        if (Mode == MODE_SOLAR) {
                            setMode(MODE_SMART);
                        }
                        break;
                    case 5: // Grid relay
                        GridRelayOpen = false;
                        break;
                    case 6: // Custom button B
                        CustomButton = !CustomButton;
                        break;
                    case 7: // Custom button S
                        CustomButton = true;
                        break;                                                
                    default:
                        if (State == STATE_C) {                             // Menu option Access is set to Disabled
                            setState(STATE_C1);
                            if (!TestState) ChargeDelay = 15;               // Keep in State B for 15 seconds, so the Charge cable can be removed.
                        }
                        break;
                }

                // Reset RCM error when button is pressed
                // RCM was tripped, but RCM level is back to normal
                if (RCmon == 1 && (ErrorFlags & RCM_TRIPPED) && digitalRead(PIN_RCM_FAULT) == LOW) {
                    // Clear RCM error
                    ErrorFlags &= ~RCM_TRIPPED;
                }
                // Also light up the LCD backlight
                // BacklightTimer = BACKLIGHT;                                 // Backlight ON

            } else {
                // Switch input released
                switch (Switch) {
                    case 2: // Access Switch
                        setAccess(false);
                        break;
                    case 3: // Smart-Solar Button
                        if (RB2low != 2) {
                            if (Mode == MODE_SMART) {
                                setMode(MODE_SOLAR);
                            } else if (Mode == MODE_SOLAR) {
                                setMode(MODE_SMART);
                            }
                            ErrorFlags &= ~(NO_SUN | LESS_6A);                   // Clear All errors
                            ChargeDelay = 0;                                // Clear any Chargedelay
                            setSolarStopTimer(0);                           // Also make sure the SolarTimer is disabled.
                            MaxSumMainsTimer = 0;
                            LCDTimer = 0;
                        }
                        RB2low = 0;
                        break;
                    case 4: // Smart-Solar Switch
                        if (Mode == MODE_SMART) setMode(MODE_SOLAR);
                        break;
                    case 5: // Grid relay
                        GridRelayOpen = true;
                        break;
                    case 6: // Custom button B
                        break;
                    case 7: // Custom button S
                        CustomButton = false;
                        break;                          
                    default:
                        break;
                }
            }

            RB2count = 0;
        }
    } else RB2count = 0;

    // Residual current monitor active, and DC current > 6mA ?
    if (RCmon == 1 && digitalRead(PIN_RCM_FAULT) == HIGH) {                   
        delay(1);
        // check again, to prevent voltage spikes from tripping the RCM detection
        if (digitalRead(PIN_RCM_FAULT) == HIGH) {                           
            if (State) setState(STATE_B1);
            ErrorFlags = RCM_TRIPPED;
            LCDTimer = 0;                                                   // display the correct error message on the LCD
        }
    }


}

#if SMARTEVSE_VERSION == 3
void getButtonState() {
    // Sample the three < o > buttons.
    // As the buttons are shared with the SPI lines going to the LCD,
    // we have to make sure that this does not interfere by write actions to the LCD.
    // Therefore updating the LCD is also done in this task.

    pinMatrixOutDetach(PIN_LCD_SDO_B3, false, false);       // disconnect MOSI pin
    pinMode(PIN_LCD_SDO_B3, INPUT);
    pinMode(PIN_LCD_A0_B2, INPUT);
    // sample buttons                                       < o >
    if (digitalRead(PIN_LCD_SDO_B3)) ButtonState = 4;       // > (right)
    else ButtonState = 0;
    if (digitalRead(PIN_LCD_A0_B2)) ButtonState |= 2;       // o (middle)
    if (digitalRead(PIN_IO0_B1)) ButtonState |= 1;          // < (left)

    pinMode(PIN_LCD_SDO_B3, OUTPUT);
    pinMatrixOutAttach(PIN_LCD_SDO_B3, VSPID_IN_IDX, false, false); // re-attach MOSI pin
    pinMode(PIN_LCD_A0_B2, OUTPUT);
}
#else
void getButtonState() {
    // Sample the three < o > buttons.

    pinMode(LCD_A0_B2, INPUT_PULLUP);                   // Switch the shared pin for the middle button to input
    if (digitalRead(BUTTON3)) ButtonState = 4;          // > (right)
    else ButtonState = 0;
    // sample the middle button
    if (digitalRead(LCD_A0_B2)) ButtonState |= 2;       // o (middle)
    pinMode(LCD_A0_B2, OUTPUT);                         // switch pin back to output
    if (digitalRead(BUTTON1)) ButtonState |= 1;         // < (left)
}
#endif

// Task that handles EVSE State Changes
// Reads buttons, and updates the LCD.
//
// called every 10ms
void Timer10ms(void * parameter) {
    uint16_t old_sec = 0;
#if SMARTEVSE_VERSION == 3
    uint8_t DiodeCheck = 0; 
    uint16_t StateTimer = 0;                                                 // When switching from State B to C, make sure pilot is at 6v for 100ms 
#else
    uint8_t RXbyte, idx = 0;
    char SerialBuf[256];
    uint8_t CommState = COMM_VER_REQ;
    uint8_t CommTimeout = 0;
    char *ret;
    uint8_t State = 0, NewState = 0;
#endif
    // infinite loop
    while(1) { 
    
        getButtonState();

        // When one or more button(s) are pressed, we call GLCDMenu
        if (((ButtonState != 0x07) || (ButtonState != OldButtonState)) ) {
            // RCM was tripped, but RCM level is back to normal
            if (getItemValue(MENU_RCMON) == 1 && (ErrorFlags & RCM_TRIPPED) && RCMFAULT == LOW) {
                ErrorFlags &= ~RCM_TRIPPED;         // Clear RCM error bit
            }
            if (!LCDlock) GLCDMenu(ButtonState);    // LCD is unlocked, enter menu
        }
        // Update/Show Helpmenu
        if (LCDNav > MENU_ENTER && LCDNav < MENU_EXIT && (ScrollTimer + 5000 < millis() ) && (!SubMenu)) GLCDHelp();

        if (timeinfo.tm_sec != old_sec) {
            old_sec = timeinfo.tm_sec;
            GLCD();
        }

#if SMARTEVSE_VERSION == 3
        // Check the external switch and RCM sensor
        CheckSwitch();

        // sample the Pilot line
        pilot = Pilot();

        // ############### EVSE State A #################

        if (State == STATE_A || State == STATE_COMM_B || State == STATE_B1) {
            // When the pilot line is disconnected, wait for PilotDisconnectTime, then reconnect
            if (PilotDisconnected) {
                if (PilotDisconnectTime == 0 && pilot == PILOT_NOK ) {          // Pilot should be ~ 0V when disconnected
                    PILOT_CONNECTED;
                    PilotDisconnected = false;
                    _LOG_A("Pilot Connected\n");
                }
            } else if (pilot == PILOT_12V) {                                    // Check if we are disconnected, or forced to State A, but still connected to the EV
                // If the RFID reader is set to EnableOne or EnableAll mode, and the Charging cable is disconnected
                // We start a timer to re-lock the EVSE (and unlock the cable) after 60 seconds.
                if ((RFIDReader == 2 || RFIDReader == 1) && AccessTimer == 0 && Access_bit == 1) AccessTimer = RFIDLOCKTIME;

                if (State != STATE_A) setState(STATE_A);                        // reset state, incase we were stuck in STATE_COMM_B
                ChargeDelay = 0;                                                // Clear ChargeDelay when disconnected.

                if (!EVMeter.ResetKwh) EVMeter.ResetKwh = 1;                    // when set, reset EV kWh meter on state B->C change.
            } else if ( pilot == PILOT_9V && ErrorFlags == NO_ERROR 
                && ChargeDelay == 0 && Access_bit && State != STATE_COMM_B
#if MODEM
                && State != STATE_MODEM_REQUEST && State != STATE_MODEM_WAIT && State != STATE_MODEM_DONE   // switch to State B ?
#endif
                    )
            {                                                                    // Allow to switch to state C directly if STATE_A_TO_C is set to PILOT_6V (see EVSE.h)
                DiodeCheck = 0;

                ProximityPin();                                                 // Sample Proximity Pin

                _LOG_I("Cable limit: %uA  Max: %uA\n", MaxCapacity, MaxCurrent);
                if (MaxCurrent > MaxCapacity) ChargeCurrent = MaxCapacity * 10; // Do not modify Max Cable Capacity or MaxCurrent (fix 2.05)
                else ChargeCurrent = MinCurrent * 10;                           // Instead use new variable ChargeCurrent

                // Load Balancing : Node
                if (LoadBl > 1) {                                               // Send command to Master, followed by Max Charge Current
                    setState(STATE_COMM_B);                                     // Node wants to switch to State B

                // Load Balancing: Master or Disabled
                } else if (IsCurrentAvailable()) {                             
                    BalancedMax[0] = MaxCapacity * 10;
                    Balanced[0] = ChargeCurrent;                                // Set pilot duty cycle to ChargeCurrent (v2.15)
#if MODEM
                    if (ModemStage == 0)
                        setState(STATE_MODEM_REQUEST);
                    else
#endif
                        setState(STATE_B);                                          // switch to State B
                    ActivationMode = 30;                                        // Activation mode is triggered if state C is not entered in 30 seconds.
                    AccessTimer = 0;
                } else if (Mode == MODE_SOLAR) {                                // Not enough power:
                    ErrorFlags |= NO_SUN;                                       // Not enough solar power
                } else ErrorFlags |= LESS_6A;                                   // Not enough power available
            } else if (pilot == PILOT_9V && State != STATE_B1 && State != STATE_COMM_B && Access_bit) {
                setState(STATE_B1);
            }
        } // State == STATE_A || State == STATE_COMM_B || State == STATE_B1

        if (State == STATE_COMM_B_OK) {
            setState(STATE_B);
            ActivationMode = 30;                                                // Activation mode is triggered if state C is not entered in 30 seconds.
            AccessTimer = 0;
        }

        // ############### EVSE State B #################
        
        if (State == STATE_B || State == STATE_COMM_C) {
        
            if (pilot == PILOT_12V) {                                           // Disconnected?
                setState(STATE_A);                                              // switch to STATE_A

            } else if (pilot == PILOT_6V && ++StateTimer > 50) {                // When switching from State B to C, make sure pilot is at 6V for at least 500ms 
                                                                                // Fixes https://github.com/dingo35/SmartEVSE-3.5/issues/40
                if ((DiodeCheck == 1) && (ErrorFlags == NO_ERROR) && (ChargeDelay == 0)) {
                    if (EVMeter.Type && EVMeter.ResetKwh) {
                        EVMeter.EnergyMeterStart = EVMeter.Energy;              // store kwh measurement at start of charging.
                        EVMeter.EnergyCharged = EVMeter.Energy - EVMeter.EnergyMeterStart; // Calculate Energy
                        EVMeter.ResetKwh = 0;                                   // clear flag, will be set when disconnected from EVSE (State A)
                    }

                    // Load Balancing : Node
                    if (LoadBl > 1) {                                           
                        if (State != STATE_COMM_C) setState(STATE_COMM_C);      // Send command to Master, followed by Charge Current

                    // Load Balancing: Master or Disabled
                    } else { 
                        BalancedMax[0] = ChargeCurrent;
                        if (IsCurrentAvailable()) {

                            Balanced[0] = 0;                                    // For correct baseload calculation set current to zero
                            CalcBalancedCurrent(1);                             // Calculate charge current for all connected EVSE's

                            DiodeCheck = 0;                                     // (local variable)
                            setState(STATE_C);                                  // switch to STATE_C
                            if (!LCDNav) GLCD();                                // Don't update the LCD if we are navigating the menu
                                                                                // immediately update LCD (20ms)
                        }
                        else if (Mode == MODE_SOLAR) {                          // Not enough power:
                            ErrorFlags |= NO_SUN;                               // Not enough solar power
                        } else ErrorFlags |= LESS_6A;                           // Not enough power available
                    }
                }

            // PILOT_9V
            } else if (pilot == PILOT_9V) {

                StateTimer = 0;                                                 // Reset State B->C transition timer
                if (ActivationMode == 0) {
                    setState(STATE_ACTSTART);
                    ActivationTimer = 3;

                    SetCPDuty(0);                                               // PWM off,  channel 0, duty cycle 0%
                                                                                // Control pilot static -12V
                }
            }
            if (pilot == PILOT_DIODE) {
                DiodeCheck = 1;                                                 // Diode found, OK
                _LOG_A("Diode OK\n");
                timerAlarmWrite(timerA, PWM_5, false);                          // Enable Timer alarm, set to start of CP signal (5%)
            }    

        }

        // ############### EVSE State C1 #################

        if (State == STATE_C1)
        {
            if (pilot == PILOT_12V) 
            {                                                                   // Disconnected or connected to EV without PWM
                setState(STATE_A);                                              // switch to STATE_A
                GLCD_init();                                                    // Re-init LCD
            }
            else if (pilot == PILOT_9V)
            {
                setState(STATE_B1);                                             // switch to State B1
                GLCD_init();                                                    // Re-init LCD
            }
        }


        if (State == STATE_ACTSTART && ActivationTimer == 0) {
            setState(STATE_B);                                                  // Switch back to State B
            ActivationMode = 255;                                               // Disable ActivationMode
        }

        if (State == STATE_COMM_C_OK) {
            DiodeCheck = 0;
            setState(STATE_C);                                                  // switch to STATE_C
                                                                                // Don't update the LCD if we are navigating the menu
            if (!LCDNav) GLCD();                                                // immediately update LCD
        }

        // ############### EVSE State C #################

        if (State == STATE_C) {
        
            if (pilot == PILOT_12V) {                                           // Disconnected ?
                setState(STATE_A);                                              // switch back to STATE_A
                GLCD_init();                                                    // Re-init LCD; necessary because switching contactors can cause LCD to mess up
    
            } else if (pilot == PILOT_9V) {
                setState(STATE_B);                                              // switch back to STATE_B
                DiodeCheck = 0;
                GLCD_init();                                                    // Re-init LCD (200ms delay); necessary because switching contactors can cause LCD to mess up
                                                                                // Mark EVSE as inactive (still State B)
            } else if (pilot != PILOT_6V) {                                     // Pilot level at anything else is an error
                if (++StateTimer > 50) {                                        // make sure it's not a glitch, by delaying by 500mS (re-using StateTimer here)
                    StateTimer = 0;                                             // Reset StateTimer for use in State B
                    setState(STATE_B);
                    DiodeCheck = 0;
                    GLCD_init();                                                // Re-init LCD (200ms delay); necessary because switching contactors can cause LCD to mess up
                } 

            } else StateTimer = 0;
    
        } // end of State C code
      
#else //SMARTEVSE_VERSION

        if (Serial1.available()) {
            //Serial.printf("[<-] ");        // Data available from mainboard?
            while (Serial1.available()) {
                RXbyte = Serial1.read();
                //Serial.printf("%c",RXbyte);
                SerialBuf[idx] = RXbyte;
                idx++;
            }
            SerialBuf[idx] = '\0'; //null terminate
            _LOG_D("[<-] %s.\n", SerialBuf);
        }
        // process data from mainboard
        if (idx > 5) {
            if (memcmp(SerialBuf, "!Panic", 6) == 0) PowerPanicESP();

            ret = strstr(SerialBuf, "Pilot:");
            //  [<-] Pilot:6,State:0,ChargeDelay:0,Error:0,Temp:34,Lock:0,Mode:0,Access:1
            if (ret != NULL) {
                int hit = sscanf(SerialBuf, "Pilot:%u,State:%u,ChargeDelay:%u,Error:%u,Temp:%i,Lock:%u,Mode:%u, Access:%u", &pilot, &State, &ChargeDelay, &ErrorFlags, &TempEVSE, &Lock, &Mode, &Access_bit);
                if (hit != 8)
                    _LOG_A("ERROR parsing line from WCH, hit=%i, line=%s.\n", hit, SerialBuf);
                else {
                    _LOG_A("DINGO: pilot=%u, State=%u, ChargeDelay=%u, ErrorFlags = %u, TempEVSE=%i, Lock=%u, Mode=%u, Access_bit=%i.\n", pilot, State,ChargeDelay, ErrorFlags, TempEVSE, Lock, Mode, Access_bit);
                }
            }

            ret = strstr(SerialBuf, "version:");
            if (ret != NULL) {
                MainVersion = atoi(ret+8);
                Serial.printf("version %u received\n", MainVersion);
                CommState = COMM_CONFIG_SET;
            }

            ret = strstr(SerialBuf, "Config:OK");
            if (ret != NULL) {
                Serial.printf("Config set\n");
                CommState = COMM_STATUS_REQ;
            }

            ret = strstr(SerialBuf, "State:"); // current State (request) received from Wch
            if (ret != NULL ) {
                State = atoi(ret+6);

                if (State == STATE_COMM_B) NewState = STATE_COMM_B_OK;
                else if (State == STATE_COMM_C) NewState = STATE_COMM_C_OK;

                if (NewState) {    // only send confirmation when state needs to change.
                    Serial1.printf("WchState:%u\n",NewState );        // send confirmation back to WCH
                    Serial.printf("[->] WchState:%u\n",NewState );    // send confirmation back to WCH
                    NewState = 0;
                }
            } else {                                                            // unformatted message must be debug message, print it!
                _LOG_V("WCH message:%s.\n,", SerialBuf);
            }
            memset(SerialBuf,0,idx);        // Clear buffer
            idx = 0;
        }

        if (CommTimeout == 0) {
            switch (CommState) {

                case COMM_VER_REQ:
                    CommTimeout = 10;
                    Serial1.print("version?\n");            // send command to WCH ic
                    Serial.print("[->] version?\n");        // send command to WCH ic
                    break;

                case COMM_CONFIG_SET:                       // Set mainboard configuration
                    CommTimeout = 10;
                    // send configuration to WCH IC
                    Serial1.printf("Config:%u,Lock:%u,Mode:%u,LoadBl:%u,Current:%u,Switch:%u,RCmon:%u,PwrPanic:%u,RFIDReader:%u,ModemPwr:%u,Initialized:%u\n", Config, Lock, Mode, LoadBl, ChargeCurrent, Switch, RCmon, PwrPanic, RFIDReader, ModemPwr, Initialized);
                    Serial.printf("[->] Config:%u,Lock:%u,Mode:%u,LoadBl:%u,Current:%u,Switch:%u,RCmon:%u,PwrPanic:%u,RFIDReader:%u,ModemPwr:%u\n", Config, Lock, Mode, LoadBl, ChargeCurrent, Switch, RCmon, PwrPanic, RFIDReader, ModemPwr);
                    break;

                case COMM_STATUS_REQ:                       // Ready to receive status from mainboard
                    CommTimeout = 10;
                    /*
                    State: A
                    Temp: 28
                    Error: 0
                    */
            }
        }


        if (CommTimeout) CommTimeout--;

#endif //SMARTEVSE_VERSION

        // Pause the task for 10ms
        vTaskDelay(10 / portTICK_PERIOD_MS);
    } // while(1) loop
}

/**
 * Send Energy measurement request over modbus
 *
 * @param uint8_t Meter
 * @param uint8_t Address
 * @param bool    Export (if exported energy is requested)
 */
void requestEnergyMeasurement(uint8_t Meter, uint8_t Address, bool Export) {
    uint8_t Count = 1;                                                          // by default it only takes 1 register to get the energy measurement
    uint16_t Register = EMConfig[Meter].ERegister;
    if (Export)
        Register = EMConfig[Meter].ERegister_Exp;

    switch (Meter) {
        case EM_FINDER_7E:
        case EM_EASTRON3P:
        case EM_EASTRON1P:
        case EM_WAGO:
            break;
        case EM_SOLAREDGE:
            // Note:
            // - SolarEdge uses 16-bit values, except for this measurement it uses 32bit int format
            // - EM_SOLAREDGE should not be used for EV Energy Measurements
            // fallthrough
        case EM_SINOTIMER:
            // Note:
            // - Sinotimer uses 16-bit values, except for this measurement it uses 32bit int format
            // fallthrough
        case EM_ABB:
            // Note:
            // - ABB uses 64bit values for this register (size 2)
            Count = 2;
            break;
        case EM_EASTRON3P_INV:
            if (Export)
                Register = EMConfig[Meter].ERegister;
            else
                Register = EMConfig[Meter].ERegister_Exp;
            break;
        default:
            if (Export)
                Count = 0; //refuse to do a request on exported energy if the meter doesnt support it
            break;
    }
    if (Count)
        requestMeasurement(Meter, Address, Register, Count);
}

/**
 * Send Power measurement request over modbus
 *
 * @param uint8_t Meter
 * @param uint8_t Address
 */
void requestPowerMeasurement(uint8_t Meter, uint8_t Address, uint16_t PRegister) {
    uint8_t Count = 1;                                                          // by default it only takes 1 register to get power measurement
    switch (Meter) {
        case EM_SINOTIMER:
            // Note:
            // - Sinotimer does not output total power but only individual power of the 3 phases
            Count = 3;
            break;
    }
    requestMeasurement(Meter, Address, PRegister, Count);
}


// Task that handles the Cable Lock
// 
// called every 100ms
//
void Timer100ms(void * parameter) {
    unsigned int locktimer = 0, unlocktimer = 0;

    while(1)  // infinite loop
    {
        // Check if the cable lock is used
        if (!Config && Lock) {                                      // Socket used and Cable lock enabled?
            // UnlockCable takes precedence over LockCable
            if ((RFIDReader == 2 && Access_bit == 0) ||             // One RFID card can Lock/Unlock the charging socket (like a public charging station)
#if ENABLE_OCPP
            (OcppMode &&!OcppForcesLock) ||
#endif
                State == STATE_A) {                                 // The charging socket is unlocked when unplugged from the EV
                if (unlocktimer == 0) {                             // 600ms pulse
                    ACTUATOR_UNLOCK;
                } else if (unlocktimer == 6) {
                    ACTUATOR_OFF;
                }
                if (unlocktimer++ > 7) {
                    if (digitalRead(PIN_LOCK_IN) == lock1 )         // still locked...
                    {
                        if (unlocktimer > 50) unlocktimer = 0;      // try to unlock again in 5 seconds
                    } else unlocktimer = 7;
                }
                locktimer = 0;
            // Lock Cable    
            } else if (State != STATE_A                            // Lock cable when connected to the EV
#if ENABLE_OCPP
            || (OcppMode && OcppForcesLock)
#endif
            ) {
                if (locktimer == 0) {                               // 600ms pulse
                    ACTUATOR_LOCK;
                } else if (locktimer == 6) {
                    ACTUATOR_OFF;
                }
                if (locktimer++ > 7) {
                    if (digitalRead(PIN_LOCK_IN) == lock2 )         // still unlocked...
                    {
                        if (locktimer > 50) locktimer = 0;          // try to lock again in 5 seconds
                    } else locktimer = 7;
                }
                unlocktimer = 0;
            }
        }

        // Pause the task for 100ms
        vTaskDelay(100 / portTICK_PERIOD_MS);

    } //while(1) loop

}

// Sequentially call the Mains/EVmeters, and polls Nodes
// Called by MBHandleError, and MBHandleData response functions.
// Once every two seconds started by Timer1s()
//
void ModbusRequestLoop() {

    static uint8_t PollEVNode = NR_EVSES;
    static uint16_t energytimer = 0;
    uint8_t updated = 0;
    
        // Every 2 seconds, request measurements from modbus meters
        // Slaves all have ModbusRequest at 0 so they never enter here
            switch (ModbusRequest) {                                            // State
                case 1:                                                         // PV kwh meter
                    ModbusRequest++;
                    // fall through
                case 2:                                                         // Sensorbox or kWh meter that measures -all- currents
                    if (MainsMeter.Type && MainsMeter.Type != EM_API) {         // we don't want modbus meter currents to conflict with EM_API currents
                        _LOG_D("ModbusRequest %u: Request MainsMeter Measurement\n", ModbusRequest);
                        requestCurrentMeasurement(MainsMeter.Type, MainsMeter.Address);
                        break;
                    }
                    ModbusRequest++;
                    // fall through
                case 3:
                    // Find next online SmartEVSE
                    do {
                        PollEVNode++;
                        if (PollEVNode >= NR_EVSES) PollEVNode = 0;
                    } while(!Node[PollEVNode].Online);

                    // Request Configuration if changed
                    if (Node[PollEVNode].ConfigChanged) {
                        _LOG_D("ModbusRequest %u: Request Configuration Node %u\n", ModbusRequest, PollEVNode);
                        // This will do the following:
                        // - Send a modbus request to the Node for it's EVmeter
                        // - Node responds with the Type and Address of the EVmeter
                        // - Master writes configuration flag reset value to Node
                        // - Node acks with the exact same message
                        // This takes around 50ms in total
                        requestNodeConfig(PollEVNode);
                        break;
                    }
                    ModbusRequest++;
                    // fall through
                case 4:                                                         // EV kWh meter, Energy measurement (total charged kWh)
                    // Request Energy if EV meter is configured
                    if (Node[PollEVNode].EVMeter && Node[PollEVNode].EVMeter != EM_API) {
                        _LOG_D("ModbusRequest %u: Request Energy Node %u\n", ModbusRequest, PollEVNode);
                        requestEnergyMeasurement(Node[PollEVNode].EVMeter, Node[PollEVNode].EVAddress, 0);
                        break;
                    }
                    ModbusRequest++;
                    // fall through
                case 5:                                                         // EV kWh meter, Power measurement (momentary power in Watt)
                    // Request Power if EV meter is configured
                    if (Node[PollEVNode].EVMeter && Node[PollEVNode].EVMeter != EM_API) {
                        updated = 1;
                        switch(EVMeter.Type) {
                            //these meters all have their power measured via receiveCurrentMeasurement already
                            case EM_EASTRON1P:
                            case EM_EASTRON3P:
                            case EM_EASTRON3P_INV:
                            case EM_ABB:
                            case EM_FINDER_7M:
                                updated = 0;
                                break;
                            default:
                                requestPowerMeasurement(Node[PollEVNode].EVMeter, Node[PollEVNode].EVAddress,EMConfig[Node[PollEVNode].EVMeter].PRegister);
                                break;
                        }
                        if (updated) break;  // do not break when EVmeter is one of the above types
                    }
                    ModbusRequest++;
                    // fall through
                case 6:                                                         // Node 1
                case 7:
                case 8:
                case 9:
                case 10:
                case 11:
                case 12:
                    if (LoadBl == 1) {
                        requestNodeStatus(ModbusRequest - 5u);                   // Master, Request Node 1-8 status
                        break;
                    }
                    ModbusRequest = 13;
                    // fall through
                case 13:
                case 14:
                case 15:
                case 16:
                case 17:
                case 18:
                case 19:
                    // Here we write State, Error, Mode and SolarTimer to Online Nodes
                    updated = 0;
                    if (LoadBl == 1) {
                        do {       
                            if (Node[ModbusRequest - 12u].Online) {             // Skip if not online
                                if (processAllNodeStates(ModbusRequest - 12u) ) {
                                    updated = 1;                                // Node updated 
                                    break;
                                }
                            }
                        } while (++ModbusRequest < 20);

                    } else ModbusRequest = 20;
                    if (updated) break;  // break when Node updated
                    // fall through
                case 20:                                                         // EV kWh meter, Current measurement
                    // Request Current if EV meter is configured
                    if (Node[PollEVNode].EVMeter && Node[PollEVNode].EVMeter != EM_API) {
                        _LOG_D("ModbusRequest %u: Request EVMeter Current Measurement Node %u\n", ModbusRequest, PollEVNode);
                        requestCurrentMeasurement(Node[PollEVNode].EVMeter, Node[PollEVNode].EVAddress);
                        break;
                    }
                    ModbusRequest++;
                    // fall through
                case 21:
                    // Request active energy if Mainsmeter is configured
                    if (MainsMeter.Type && (MainsMeter.Type != EM_API) && (MainsMeter.Type != EM_SENSORBOX) ) { // EM_API and Sensorbox do not support energy postings
                        energytimer++; //this ticks approx every second?!?
                        if (energytimer == 30) {
                            _LOG_D("ModbusRequest %u: Request MainsMeter Import Active Energy Measurement\n", ModbusRequest);
                            requestEnergyMeasurement(MainsMeter.Type, MainsMeter.Address, 0);
                            break;
                        }
                        if (energytimer >= 60) {
                            _LOG_D("ModbusRequest %u: Request MainsMeter Export Active Energy Measurement\n", ModbusRequest);
                            requestEnergyMeasurement(MainsMeter.Type, MainsMeter.Address, 1);
                            energytimer = 0;
                            break;
                        }
                    }
                    ModbusRequest++;
                    // fall through
                default:
                    // slave never gets here
                    // what about normal mode with no meters attached?
                    CalcBalancedCurrent(0);
                    // No current left, or Overload (2x Maxmains)?
                    if (Mode && (NoCurrent > 2 || MainsMeter.Imeasured > (MaxMains * 20))) { // I guess we don't want to set this flag in Normal mode, we just want to charge ChargeCurrent
                        // STOP charging for all EVSE's
                        // Display error message
                        ErrorFlags |= LESS_6A; //NOCURRENT;
                        // Broadcast Error code over RS485
                        ModbusWriteSingleRequest(BROADCAST_ADR, 0x0001, ErrorFlags);
                        NoCurrent = 0;
                    }
                    if (LoadBl == 1 && !(ErrorFlags & CT_NOCOMM) ) BroadcastCurrent();               // When there is no Comm Error, Master sends current to all connected EVSE's

                    if ((State == STATE_B || State == STATE_C) && !CPDutyOverride) SetCurrent(Balanced[0]); // set PWM output for Master //mind you, the !CPDutyOverride was not checked in Smart/Solar mode, but I think this was a bug!
                    printStatus();  //for debug purposes
                    ModbusRequest = 0;
                    //_LOG_A("Timer100ms task free ram: %u\n", uxTaskGetStackHighWaterMark( NULL ));
                    break;
            } //switch
            if (ModbusRequest) ModbusRequest++;
}


#if MQTT
void mqtt_receive_callback(const String topic, const String payload) {
    if (topic == MQTTprefix + "/Set/Mode") {
        if (payload == "Off") {
            ToModemWaitStateTimer = 0;
            ToModemDoneStateTimer = 0;
            LeaveModemDoneStateTimer = 0;
            setAccess(0);
        } else if (payload == "Normal") {
            setMode(MODE_NORMAL);
        } else if (payload == "Solar") {
            OverrideCurrent = 0;
            setMode(MODE_SOLAR);
        } else if (payload == "Smart") {
            OverrideCurrent = 0;
            setMode(MODE_SMART);
        }
    } else if (topic == MQTTprefix + "/Set/CustomButton") {
        if (payload == "On") {
            CustomButton = true;
        } else {
            CustomButton = false;
        }
    } else if (topic == MQTTprefix + "/Set/CurrentOverride") {
        uint16_t RequestedCurrent = payload.toInt();
        if (RequestedCurrent == 0) {
            OverrideCurrent = 0;
        } else if (LoadBl < 2 && (Mode == MODE_NORMAL || Mode == MODE_SMART)) { // OverrideCurrent not possible on Slave
            if (RequestedCurrent >= (MinCurrent * 10) && RequestedCurrent <= (MaxCurrent * 10)) {
                OverrideCurrent = RequestedCurrent;
            }
        }
    } else if (topic == MQTTprefix + "/Set/CurrentMaxSumMains" && LoadBl < 2) {
        uint16_t RequestedCurrent = payload.toInt();
        if (RequestedCurrent == 0) {
            MaxSumMains = 0;
        } else if (RequestedCurrent == 0 || (RequestedCurrent >= 10 && RequestedCurrent <= 600)) {
                MaxSumMains = RequestedCurrent;
        }
    } else if (topic == MQTTprefix + "/Set/CPPWMOverride") {
        int pwm = payload.toInt();
        if (pwm == -1) {
            SetCPDuty(1024);
            CP_ON;
            CPDutyOverride = false;
        } else if (pwm == 0) {
            SetCPDuty(0);
            CP_OFF;
            CPDutyOverride = true;
        } else if (pwm <= 1024) {
            SetCPDuty(pwm);
            CP_ON;
            CPDutyOverride = true;
        }
    } else if (topic == MQTTprefix + "/Set/MainsMeter") {
        if (MainsMeter.Type != EM_API || LoadBl >= 2)
            return;

        int32_t L1, L2, L3;
        int n = sscanf(payload.c_str(), "%d:%d:%d", &L1, &L2, &L3);

        // MainsMeter can measure -200A to +200A per phase
        if (n == 3 && (L1 > -2000 && L1 < 2000) && (L2 > -2000 && L2 < 2000) && (L3 > -2000 && L3 < 2000)) {
            if (LoadBl < 2)
                MainsMeter.Timeout = COMM_TIMEOUT;
            MainsMeter.Irms[0] = L1;
            MainsMeter.Irms[1] = L2;
            MainsMeter.Irms[2] = L3;
            CalcIsum();
        }
    } else if (topic == MQTTprefix + "/Set/EVMeter") {
        if (EVMeter.Type != EM_API)
            return;

        int32_t L1, L2, L3, W, WH;
        int n = sscanf(payload.c_str(), "%d:%d:%d:%d:%d", &L1, &L2, &L3, &W, &WH);

        // We expect 5 values (and accept -1 for unknown values)
        if (n == 5) {
            if ((L1 > -1 && L1 < 1000) && (L2 > -1 && L2 < 1000) && (L3 > -1 && L3 < 1000)) {
                // RMS currents
                EVMeter.Irms[0] = L1;
                EVMeter.Irms[1] = L2;
                EVMeter.Irms[2] = L3;
                EVMeter.CalcImeasured();
                EVMeter.Timeout = COMM_EVTIMEOUT;
            }

            if (W > -1) {
                // Power measurement
                EVMeter.PowerMeasured = W;
            }

            if (WH > -1) {
                // Energy measurement
                EVMeter.Import_active_energy = WH;
                EVMeter.Export_active_energy = 0;
                EVMeter.UpdateEnergies();
            }
        }
    } else if (topic == MQTTprefix + "/Set/HomeBatteryCurrent") {
        if (LoadBl >= 2)
            return;
        homeBatteryCurrent = payload.toInt();
        homeBatteryLastUpdate = time(NULL);
    } else if (topic == MQTTprefix + "/Set/RequiredEVCCID") {
        strncpy(RequiredEVCCID, payload.c_str(), sizeof(RequiredEVCCID));
        if (preferences.begin("settings", false) ) {                        //false = write mode
            preferences.putString("RequiredEVCCID", String(RequiredEVCCID));
            preferences.end();
        }
    } else if (topic == MQTTprefix + "/Set/ColorOff") {
        int32_t R, G, B;
        int n = sscanf(payload.c_str(), "%d,%d,%d", &R, &G, &B);

        // R,G,B is between 0..255
        if (n == 3 && (R >= 0 && R < 256) && (G >= 0 && G < 256) && (B >= 0 && B < 256)) {
            ColorOff[0] = R;
            ColorOff[1] = G;
            ColorOff[2] = B;
        }
    } else if (topic == MQTTprefix + "/Set/ColorNormal") {
        int32_t R, G, B;
        int n = sscanf(payload.c_str(), "%d,%d,%d", &R, &G, &B);

        // R,G,B is between 0..255
        if (n == 3 && (R >= 0 && R < 256) && (G >= 0 && G < 256) && (B >= 0 && B < 256)) {
            ColorNormal[0] = R;
            ColorNormal[1] = G;
            ColorNormal[2] = B;
        }
    } else if (topic == MQTTprefix + "/Set/ColorSmart") {
        int32_t R, G, B;
        int n = sscanf(payload.c_str(), "%d,%d,%d", &R, &G, &B);

        // R,G,B is between 0..255
        if (n == 3 && (R >= 0 && R < 256) && (G >= 0 && G < 256) && (B >= 0 && B < 256)) {
            ColorSmart[0] = R;
            ColorSmart[1] = G;
            ColorSmart[2] = B;
        }
    } else if (topic == MQTTprefix + "/Set/ColorSolar") {
        int32_t R, G, B;
        int n = sscanf(payload.c_str(), "%d,%d,%d", &R, &G, &B);

        // R,G,B is between 0..255
        if (n == 3 && (R >= 0 && R < 256) && (G >= 0 && G < 256) && (B >= 0 && B < 256)) {
            ColorSolar[0] = R;
            ColorSolar[1] = G;
            ColorSolar[2] = B;
        }
    } else if (topic == MQTTprefix + "/Set/ColorCustom") {
        int32_t R, G, B;
        int n = sscanf(payload.c_str(), "%d,%d,%d", &R, &G, &B);

        // R,G,B is between 0..255
        if (n == 3 && (R >= 0 && R < 256) && (G >= 0 && G < 256) && (B >= 0 && B < 256)) {
            ColorCustom[0] = R;
            ColorCustom[1] = G;
            ColorCustom[2] = B;
        }
    }

    // Make sure MQTT updates directly to prevent debounces
    lastMqttUpdate = 10;
}


void SetupMQTTClient() {
    // Set up subscriptions
    MQTTclient.subscribe(MQTTprefix + "/Set/#",1);
    MQTTclient.publish(MQTTprefix+"/connected", "online", true, 0);

    //publish MQTT discovery topics
    //we need something to make all this JSON stuff readable, without doing all this assign and serialize stuff
#define jsn(x, y) String(R"(")") + x + R"(" : ")" + y + R"(")"
    //jsn(device_class, current) expands to:
    // R"("device_class" : "current")"

#define jsna(x, y) String(R"(, )") + jsn(x, y)
    //json add expansion, same as above but now with a comma prepended

    //first all device stuff:
    const String device_payload = String(R"("device": {)") + jsn("model","SmartEVSE v3") + jsna("identifiers", MQTTprefix) + jsna("name", MQTTprefix) + jsna("manufacturer","Stegen") + jsna("configuration_url", "http://" + WiFi.localIP().toString().c_str()) + jsna("sw_version", String(VERSION)) + "}";
    //a device SmartEVSE-1001 consists of multiple entities, and an entity can be in the domains sensor, number, select etc.
    String entity_suffix, entity_name, optional_payload;

    //some self-updating variables here:
#define entity_id String(MQTTprefix + "-" + entity_suffix)
#define entity_path String(MQTTprefix + "/" + entity_suffix)
#define entity_name(x) entity_name = x; entity_suffix = entity_name; entity_suffix.replace(" ", "");

    //create template to announce an entity in it's own domain:
#define announce(x, entity_domain) entity_name(x); \
    MQTTclient.publish("homeassistant/" + String(entity_domain) + "/" + entity_id + "/config", \
     "{" \
        + jsn("name", entity_name) \
        + jsna("object_id", entity_id) \
        + jsna("unique_id", entity_id) \
        + jsna("state_topic", entity_path) \
        + jsna("availability_topic",String(MQTTprefix+"/connected")) \
        + ", " + device_payload + optional_payload \
        + "}", \
    true, 0); // Retain + QoS 0

    //set the parameters for and announce sensors with device class 'current':
    optional_payload = jsna("device_class","current") + jsna("unit_of_measurement","A") + jsna("value_template", R"({{ value | int / 10 }})");
    announce("Charge Current", "sensor");
    announce("Max Current", "sensor");
    if (MainsMeter.Type) {
        announce("Mains Current L1", "sensor");
        announce("Mains Current L2", "sensor");
        announce("Mains Current L3", "sensor");
    }
    if (EVMeter.Type) {
        announce("EV Current L1", "sensor");
        announce("EV Current L2", "sensor");
        announce("EV Current L3", "sensor");
    }
    if (homeBatteryLastUpdate) {
        announce("Home Battery Current", "sensor");
    }

#if MODEM
        //set the parameters for modem/SoC sensor entities:
        optional_payload = jsna("unit_of_measurement","%") + jsna("value_template", R"({{ none if (value | int == -1) else (value | int) }})");
        announce("EV Initial SoC", "sensor");
        announce("EV Full SoC", "sensor");
        announce("EV Computed SoC", "sensor");
        announce("EV Remaining SoC", "sensor");

        optional_payload = jsna("device_class","duration") + jsna("unit_of_measurement","m") + jsna("value_template", R"({{ none if (value | int == -1) else (value | int / 60) | round }})");
        announce("EV Time Until Full", "sensor");

        optional_payload = jsna("device_class","energy") + jsna("unit_of_measurement","Wh") + jsna("value_template", R"({{ none if (value | int == -1) else (value | int) }})");
        announce("EV Energy Capacity", "sensor");
        announce("EV Energy Request", "sensor");

        optional_payload = jsna("value_template", R"({{ none if (value == '') else value }})");
        announce("EVCCID", "sensor");
        optional_payload = jsna("state_topic", String(MQTTprefix + "/RequiredEVCCID")) + jsna("command_topic", String(MQTTprefix + "/Set/RequiredEVCCID"));
        announce("Required EVCCID", "text");
#endif

    if (EVMeter.Type) {
        //set the parameters for and announce other sensor entities:
        optional_payload = jsna("device_class","power") + jsna("unit_of_measurement","W");
        announce("EV Charge Power", "sensor");
        optional_payload = jsna("device_class","energy") + jsna("unit_of_measurement","Wh");
        announce("EV Energy Charged", "sensor");
        optional_payload = jsna("device_class","energy") + jsna("unit_of_measurement","Wh") + jsna("state_class","total_increasing");
        announce("EV Total Energy Charged", "sensor");
    }

    //set the parameters for and announce sensor entities without device_class or unit_of_measurement:
    optional_payload = "";
    announce("EV Plug State", "sensor");
    announce("Access", "sensor");
    announce("State", "sensor");
    announce("RFID", "sensor");
    announce("RFIDLastRead", "sensor");

#if ENABLE_OCPP
    announce("OCPP", "sensor");
    announce("OCPPConnection", "sensor");
#endif //ENABLE_OCPP

    optional_payload = jsna("state_topic", String(MQTTprefix + "/LEDColorOff")) + jsna("command_topic", String(MQTTprefix + "/Set/ColorOff"));
    announce("LED Color Off", "text");
    optional_payload = jsna("state_topic", String(MQTTprefix + "/LEDColorNormal")) + jsna("command_topic", String(MQTTprefix + "/Set/ColorNormal"));
    announce("LED Color Normal", "text");
    optional_payload = jsna("state_topic", String(MQTTprefix + "/LEDColorSmart")) + jsna("command_topic", String(MQTTprefix + "/Set/ColorSmart"));
    announce("LED Color Smart", "text");
    optional_payload = jsna("state_topic", String(MQTTprefix + "/LEDColorSolar")) + jsna("command_topic", String(MQTTprefix + "/Set/ColorSolar"));
    announce("LED Color Solar", "text");
    optional_payload = jsna("state_topic", String(MQTTprefix + "/LEDColorCustom")) + jsna("command_topic", String(MQTTprefix + "/Set/ColorCustom"));
    announce("LED Color Custom", "text");
    
    optional_payload = jsna("state_topic", String(MQTTprefix + "/CustomButton")) + jsna("command_topic", String(MQTTprefix + "/Set/CustomButton"));
    optional_payload += String(R"(, "options" : ["On", "Off"])");
    announce("Custom Button", "select");

    optional_payload = jsna("device_class","duration") + jsna("unit_of_measurement","s");
    announce("SolarStopTimer", "sensor");
    //set the parameters for and announce diagnostic sensor entities:
    optional_payload = jsna("entity_category","diagnostic");
    announce("Error", "sensor");
    announce("WiFi SSID", "sensor");
    announce("WiFi BSSID", "sensor");
    optional_payload = jsna("entity_category","diagnostic") + jsna("device_class","signal_strength") + jsna("unit_of_measurement","dBm");
    announce("WiFi RSSI", "sensor");
    optional_payload = jsna("entity_category","diagnostic") + jsna("device_class","temperature") + jsna("unit_of_measurement","°C");
    announce("ESP Temp", "sensor");
    optional_payload = jsna("entity_category","diagnostic") + jsna("device_class","duration") + jsna("unit_of_measurement","s") + jsna("entity_registry_enabled_default","False");
    announce("ESP Uptime", "sensor");

#if MODEM
        optional_payload = jsna("unit_of_measurement","%") + jsna("value_template", R"({{ (value | int / 1024 * 100) | round(0) }})");
        announce("CP PWM", "sensor");

        optional_payload = jsna("value_template", R"({{ none if (value | int == -1) else (value | int / 1024 * 100) | round }})");
        optional_payload += jsna("command_topic", String(MQTTprefix + "/Set/CPPWMOverride")) + jsna("min", "-1") + jsna("max", "100") + jsna("mode","slider");
        optional_payload += jsna("command_template", R"({{ (value | int * 1024 / 100) | round }})");
        announce("CP PWM Override", "number");
#endif
    //set the parameters for and announce select entities, overriding automatic state_topic:
    optional_payload = jsna("state_topic", String(MQTTprefix + "/Mode")) + jsna("command_topic", String(MQTTprefix + "/Set/Mode"));
    optional_payload += String(R"(, "options" : ["Off", "Normal", "Smart", "Solar"])");
    announce("Mode", "select");

    //set the parameters for and announce number entities:
    optional_payload = jsna("command_topic", String(MQTTprefix + "/Set/CurrentOverride")) + jsna("min", "0") + jsna("max", MaxCurrent ) + jsna("mode","slider");
    optional_payload += jsna("value_template", R"({{ value | int / 10 if value | is_number else none }})") + jsna("command_template", R"({{ value | int * 10 }})");
    announce("Charge Current Override", "number");
}

void mqttPublishData() {
    lastMqttUpdate = 0;

        if (MainsMeter.Type) {
            MQTTclient.publish(MQTTprefix + "/MainsCurrentL1", MainsMeter.Irms[0], false, 0);
            MQTTclient.publish(MQTTprefix + "/MainsCurrentL2", MainsMeter.Irms[1], false, 0);
            MQTTclient.publish(MQTTprefix + "/MainsCurrentL3", MainsMeter.Irms[2], false, 0);
        }
        if (EVMeter.Type) {
            MQTTclient.publish(MQTTprefix + "/EVCurrentL1", EVMeter.Irms[0], false, 0);
            MQTTclient.publish(MQTTprefix + "/EVCurrentL2", EVMeter.Irms[1], false, 0);
            MQTTclient.publish(MQTTprefix + "/EVCurrentL3", EVMeter.Irms[2], false, 0);
        }
        MQTTclient.publish(MQTTprefix + "/ESPTemp", TempEVSE, false, 0);
        MQTTclient.publish(MQTTprefix + "/Mode", Access_bit == 0 ? "Off" : Mode > 3 ? "N/A" : StrMode[Mode], true, 0);
        MQTTclient.publish(MQTTprefix + "/MaxCurrent", MaxCurrent * 10, true, 0);
        MQTTclient.publish(MQTTprefix + "/CustomButton", CustomButton ? "On" : "Off", false, 0);
        MQTTclient.publish(MQTTprefix + "/ChargeCurrent", Balanced[0], true, 0);
        MQTTclient.publish(MQTTprefix + "/ChargeCurrentOverride", OverrideCurrent, true, 0);
        MQTTclient.publish(MQTTprefix + "/Access", StrAccessBit[Access_bit], true, 0);
        MQTTclient.publish(MQTTprefix + "/RFID", !RFIDReader ? "Not Installed" : RFIDstatus >= 8 ? "NOSTATUS" : StrRFIDStatusWeb[RFIDstatus], true, 0);
        if (RFIDReader && RFIDReader != 6) { //RFIDLastRead not updated in Remote/OCPP mode
            char buf[13];
            sprintf(buf, "%02X%02X%02X%02X%02X%02X", RFID[1], RFID[2], RFID[3], RFID[4], RFID[5], RFID[6]);
            MQTTclient.publish(MQTTprefix + "/RFIDLastRead", buf, true, 0);
        }
        MQTTclient.publish(MQTTprefix + "/State", getStateNameWeb(State), true, 0);
        MQTTclient.publish(MQTTprefix + "/Error", getErrorNameWeb(ErrorFlags), true, 0);
        MQTTclient.publish(MQTTprefix + "/EVPlugState", (pilot != PILOT_12V) ? "Connected" : "Disconnected", true, 0);
        MQTTclient.publish(MQTTprefix + "/WiFiSSID", String(WiFi.SSID()), true, 0);
        MQTTclient.publish(MQTTprefix + "/WiFiBSSID", String(WiFi.BSSIDstr()), true, 0);
#if MODEM
        MQTTclient.publish(MQTTprefix + "/CPPWM", CurrentPWM, false, 0);
        MQTTclient.publish(MQTTprefix + "/CPPWMOverride", CPDutyOverride ? String(CurrentPWM) : "-1", true, 0);
        MQTTclient.publish(MQTTprefix + "/EVInitialSoC", InitialSoC, true, 0);
        MQTTclient.publish(MQTTprefix + "/EVFullSoC", FullSoC, true, 0);
        MQTTclient.publish(MQTTprefix + "/EVComputedSoC", ComputedSoC, true, 0);
        MQTTclient.publish(MQTTprefix + "/EVRemainingSoC", RemainingSoC, true, 0);
        MQTTclient.publish(MQTTprefix + "/EVTimeUntilFull", TimeUntilFull, false, 0);
        MQTTclient.publish(MQTTprefix + "/EVEnergyCapacity", EnergyCapacity, true, 0);
        MQTTclient.publish(MQTTprefix + "/EVEnergyRequest", EnergyRequest, true, 0);
        MQTTclient.publish(MQTTprefix + "/EVCCID", EVCCID, true, 0);
        MQTTclient.publish(MQTTprefix + "/RequiredEVCCID", RequiredEVCCID, true, 0);
#endif
        if (EVMeter.Type) {
            MQTTclient.publish(MQTTprefix + "/EVChargePower", EVMeter.PowerMeasured, false, 0);
            MQTTclient.publish(MQTTprefix + "/EVEnergyCharged", EVMeter.EnergyCharged, true, 0);
            MQTTclient.publish(MQTTprefix + "/EVTotalEnergyCharged", EVMeter.Energy, false, 0);
        }
        if (homeBatteryLastUpdate)
            MQTTclient.publish(MQTTprefix + "/HomeBatteryCurrent", homeBatteryCurrent, false, 0);
#if ENABLE_OCPP
        MQTTclient.publish(MQTTprefix + "/OCPP", OcppMode ? "Enabled" : "Disabled", true, 0);
        MQTTclient.publish(MQTTprefix + "/OCPPConnection", (OcppWsClient && OcppWsClient->isConnected()) ? "Connected" : "Disconnected", false, 0);
#endif //ENABLE_OCPP
        MQTTclient.publish(MQTTprefix + "/LEDColorOff", String(ColorOff[0])+","+String(ColorOff[1])+","+String(ColorOff[2]), true, 0);
        MQTTclient.publish(MQTTprefix + "/LEDColorNormal", String(ColorNormal[0])+","+String(ColorNormal[1])+","+String(ColorNormal[2]), true, 0);
        MQTTclient.publish(MQTTprefix + "/LEDColorSmart", String(ColorSmart[0])+","+String(ColorSmart[1])+","+String(ColorSmart[2]), true, 0);
        MQTTclient.publish(MQTTprefix + "/LEDColorSolar", String(ColorSolar[0])+","+String(ColorSolar[1])+","+String(ColorSolar[2]), true, 0);
        MQTTclient.publish(MQTTprefix + "/LEDColorCustom", String(ColorCustom[0])+","+String(ColorCustom[1])+","+String(ColorCustom[2]), true, 0);
}
#endif


/**
 * Set the solar stop timer
 *
 * @param unsigned int Timer (seconds)
 */
void setSolarStopTimer(uint16_t Timer) {
    if (SolarStopTimer == Timer)
        return;                                                             // prevent unnecessary publishing of SolarStopTimer
    SolarStopTimer = Timer;
#if MQTT
    MQTTclient.publish(MQTTprefix + "/SolarStopTimer", SolarStopTimer, false, 0);
#endif
}


// task 1000msTimer
void Timer1S(void * parameter) {

    uint8_t Broadcast = 1;
    //uint8_t Timer5sec = 0;
    uint8_t x;

    while(1) { // infinite loop

        if (BacklightTimer) BacklightTimer--;                               // Decrease backlight counter every second.

        // wait for Activation mode to start
        if (ActivationMode && ActivationMode != 255) {
            ActivationMode--;                                               // Decrease ActivationMode every second.
        }

        // activation Mode is active
        if (ActivationTimer) ActivationTimer--;                             // Decrease ActivationTimer every second.
#if MODEM
        if (State == STATE_MODEM_REQUEST){
            if (ToModemWaitStateTimer) ToModemWaitStateTimer--;
            else{
                setState(STATE_MODEM_WAIT);                                         // switch to state Modem 2
                GLCD();
            }
        }

        if (State == STATE_MODEM_WAIT){
            if (ToModemDoneStateTimer) ToModemDoneStateTimer--;
            else{
                setState(STATE_MODEM_DONE); 
                GLCD();
            }
        }

        if (State == STATE_MODEM_DONE){
            if (LeaveModemDoneStateTimer) LeaveModemDoneStateTimer--;
            else{
                // Here's what happens:
                //  - State STATE_MODEM_DONE set the CP pin off, to reset connection with car. Since some cars don't support AC charging via ISO15118, SoC is extracted via DC. 
                //  - Negotiation fails between pyPLC and car. Some cars then won't accept charge via AC it seems after, so we just "re-plug" and start charging without the modem communication protocol 
                //  - State STATE_B will enable CP pin again, if disabled. 
                // This stage we are now in is just before we enable CP_PIN and resume via STATE_B

                // Reset CP to idle & turn off, it will be turned on again later for another try
                SetCPDuty(1024);
                CP_OFF;

                // Check whether the EVCCID matches the one required
                if (strcmp(RequiredEVCCID, "") == 0 || strcmp(RequiredEVCCID, EVCCID) == 0) {
                    // We satisfied the EVCCID requirements, skip modem stages next time
                    ModemStage = 1;

                    setState(STATE_B);                                     // switch to STATE_B
                    GLCD();                                                // Re-init LCD (200ms delay)
                } else {
                    // We actually do not want to continue charging and re-start at modem request after 60s
                    ModemStage = 0;
                    LeaveModemDeniedStateTimer = 60;

                    // Change to MODEM_DENIED state
                    setState(STATE_MODEM_DENIED);
                    GLCD();                                                // Re-init LCD (200ms delay)
                }
            }
        }

        if (State == STATE_MODEM_DENIED){
            if (LeaveModemDeniedStateTimer) LeaveModemDeniedStateTimer--;
            else{
                LeaveModemDeniedStateTimer = -1;           // reset ModemStateDeniedTimer
                setState(STATE_A);                         // switch to STATE_B
                CP_ON;
                GLCD();                                    // Re-init LCD (200ms delay)
            }
        }

#endif
        if (State == STATE_C1) {
            if (C1Timer) C1Timer--;                                         // if the EV does not stop charging in 6 seconds, we will open the contactor.
            else {
                _LOG_A("State C1 timeout!\n");
                setState(STATE_B1);                                         // switch back to STATE_B1
                GLCD_init();                                                // Re-init LCD (200ms delay); necessary because switching contactors can cause LCD to mess up
            }
        }

#if MODEM
        // Normally, the modem is enabled when Modem == Experiment. However, after a succesfull communication has been set up, EVSE will restart communication by replugging car and moving back to state B.
        // This time, communication is not initiated. When a car is disconnected, we want to enable the modem states again, but using 12V signal is not reliable (we just "replugged" via CP pin, remember).
        // This counter just enables the state after 3 seconds of success.
        if (DisconnectTimeCounter >= 0){
            DisconnectTimeCounter++;
        }

        if (DisconnectTimeCounter > 3){
            if (pilot == PILOT_12V){
                DisconnectTimeCounter = -1;
                DisconnectEvent();
            } else{ // Run again
                DisconnectTimeCounter = 0; 
            }
        }
#endif

#if SMARTEVSE_VERSION == 3
        // once a second, measure temperature
        // range -40 .. +125C
        TempEVSE = TemperatureSensor();                                                             

#endif

        // Check if there is a RFID card in front of the reader
        CheckRFID();

                 
        // When Solar Charging, once the current drops to MINcurrent a timer is started.
        // Charging is stopped when the timer reaches the time set in 'StopTime' (in minutes)
        // Except when Stoptime =0, then charging will continue.

        if (SolarStopTimer) {
            SolarStopTimer--;
#if MQTT
            MQTTclient.publish(MQTTprefix + "/SolarStopTimer", SolarStopTimer, false, 0);
#endif
            if (SolarStopTimer == 0) {
                if (State == STATE_C) setState(STATE_C1);                   // tell EV to stop charging
                ErrorFlags |= NO_SUN;                                       // Set error: NO_SUN
            }
        }

        // When Smart or Solar Charging, once MaxSumMains is exceeded, a timer is started
        // Charging is stopped when the timer reaches the time set in 'MaxSumMainsTime' (in minutes)
        // Except when MaxSumMainsTime =0, then charging will continue.
        if (MaxSumMainsTimer) {
            MaxSumMainsTimer--;                                             // Decrease MaxSumMains counter every second.
            if (MaxSumMainsTimer == 0) {
                if (State == STATE_C) setState(STATE_C1);                   // tell EV to stop charging
                ErrorFlags |= LESS_6A;                                      // Set error: LESS_6A
            }
        }

        if (ChargeDelay) ChargeDelay--;                                     // Decrease Charge Delay counter
        if (PilotDisconnectTime) PilotDisconnectTime--;                     // Decrease PilotDisconnectTimer

        if (AccessTimer && State == STATE_A) {
            if (--AccessTimer == 0) {
                setAccess(false);                                           // re-lock EVSE
            }
        } else AccessTimer = 0;                                             // Not in state A, then disable timer

        if ((TempEVSE < (maxTemp - 10)) && (ErrorFlags & TEMP_HIGH)) {                  // Temperature below limit?
            ErrorFlags &= ~TEMP_HIGH; // clear Error
        }

        if ( (ErrorFlags & (LESS_6A|NO_SUN) ) && (LoadBl < 2) && (IsCurrentAvailable())) {
            ErrorFlags &= ~LESS_6A;                                         // Clear Errors if there is enough current available, and Load Balancing is disabled or we are Master
            ErrorFlags &= ~NO_SUN;
            _LOG_I("No sun/current Errors Cleared.\n");
        }


        // Charge timer
        for (x = 0; x < NR_EVSES; x++) {
            if (BalancedState[x] == STATE_C) {
                Node[x].IntTimer++;
                Node[x].Timer++;
             } else Node[x].IntTimer = 0;                                    // Reset IntervalTime when not charging
        }

        // Mainsmeter defined, and power sharing set to Disabled or Master
        if (MainsMeter.Type && LoadBl < 2) {
            if ( MainsMeter.Timeout == 0 && !(ErrorFlags & CT_NOCOMM) && Mode != MODE_NORMAL) { // timeout if current measurement takes > 10 secs
                // When power sharing is set to Disabled/Master, and in Normal mode, do not timeout;
                // there might be MainsMeter/EVMeter configured that can be retrieved through the API.
                ErrorFlags |= CT_NOCOMM;
                setStatePowerUnavailable();
                SB2.SoftwareVer = 0;
                _LOG_W("Error, MainsMeter communication error!\n");
            } else {
                if (MainsMeter.Timeout) MainsMeter.Timeout--;
            }
        // We are a Node, we will timeout if there is no communication with the Master controller.   
        } else if (LoadBl > 1) {
            if (MainsMeter.Timeout == 0 && !(ErrorFlags & CT_NOCOMM)) {
                ErrorFlags |= CT_NOCOMM;
                setStatePowerUnavailable();
                SB2.SoftwareVer = 0;
                _LOG_W("Error, Master communication error!\n");
            } else {
                if (MainsMeter.Timeout) MainsMeter.Timeout--;
            }    
        } else
            MainsMeter.Timeout = COMM_TIMEOUT;

        if (EVMeter.Type) {
            if ( EVMeter.Timeout == 0 && !(ErrorFlags & EV_NOCOMM) && Mode != MODE_NORMAL) {
                ErrorFlags |= EV_NOCOMM;
                setStatePowerUnavailable();
                _LOG_W("Error, EV Meter communication error!\n");
            } else {
                if (EVMeter.Timeout) EVMeter.Timeout--;
            }
        } else
            EVMeter.Timeout = COMM_EVTIMEOUT;
        
        // Clear communication error, if present
        if ((ErrorFlags & CT_NOCOMM) && MainsMeter.Timeout) ErrorFlags &= ~CT_NOCOMM;

        if ((ErrorFlags & EV_NOCOMM) && EVMeter.Timeout) ErrorFlags &= ~EV_NOCOMM;


        if (TempEVSE > maxTemp && !(ErrorFlags & TEMP_HIGH))                // Temperature too High?
        {
            ErrorFlags |= TEMP_HIGH;
            setStatePowerUnavailable();
            _LOG_W("Error, temperature %i C !\n", TempEVSE);
        }

        if (ErrorFlags & (NO_SUN | LESS_6A)) {
            if (ChargeDelay == 0) {
                if (Mode == MODE_SOLAR) { _LOG_I("Waiting for Solar power...\n"); }
                else { _LOG_I("Not enough current available!\n"); }
            }
            setStatePowerUnavailable();
            ChargeDelay = CHARGEDELAY;                                      // Set Chargedelay
        }

        // Every two seconds request measurement data from sensorbox/kwh meters.
        // and send broadcast to Node controllers.
        if (LoadBl < 2 && !Broadcast--) {                                   // Load Balancing mode: Master or Disabled
            ModbusRequest = 1;                                              // Start with state 1, also in Normal mode we want MainsMeter and EVmeter updated 
            ModbusRequestLoop();
            //timeout = COMM_TIMEOUT; not sure if necessary, statement was missing in original code    // reset timeout counter (not checked for Master)
            Broadcast = 1;                                                  // repeat every two seconds
        }

#if SMARTEVSE_VERSION == 3
        // for Slave modbusrequest loop is never called, so we have to show debug info here...
        if (LoadBl > 1)
#endif
            printStatus();  //for debug purposes

        //_LOG_A("Timer1S task free ram: %u\n", uxTaskGetStackHighWaterMark( NULL ));


#if MQTT
        if (lastMqttUpdate++ >= 10) {
            // Publish latest data, every 10 seconds
            // We will try to publish data faster if something has changed
            mqttPublishData();
        }
#endif

        // Pause the task for 1 Sec
        vTaskDelay(1000 / portTICK_PERIOD_MS);

    } // while(1)
}

// Monitor EV Meter responses, and update Enery and Power and Current measurements
// Both the Master and Nodes will receive their own EV meter measurements here.
// Does not send any data back.
//
ModbusMessage MBEVMeterResponse(ModbusMessage request) {
    ModbusDecode( (uint8_t*)request.data(), request.size());
    EVMeter.ResponseToMeasurement();
    // As this is a response to an earlier request, do not send response.
    
    return NIL_RESPONSE;              
}


// Request handler for modbus messages addressed to -this- Node/Slave EVSE.
// Sends response back to Master
//
ModbusMessage MBNodeRequest(ModbusMessage request) {
    ModbusMessage response;     // response message to be sent back
    uint8_t ItemID;
    uint8_t i, OK = 0;
    uint16_t value, values[MODBUS_MAX_REGISTER_READ];
    
    // Check if the call is for our current ServerID, or maybe for an old ServerID?
    if (LoadBl != request.getServerID()) return NIL_RESPONSE;
    

    ModbusDecode( (uint8_t*)request.data(), request.size());
    ItemID = mapModbusRegister2ItemID();

    switch (MB.Function) {
        case 0x03: // (Read holding register)
        case 0x04: // (Read input register)
            //     ReadItemValueResponse();
            if (ItemID) {
                response.add(MB.Address, MB.Function, (uint8_t)(MB.RegisterCount * 2));

                _LOG_D("Node answering NodeStatus request");
                for (i = 0; i < MB.RegisterCount; i++) {
                    values[i] = getItemValue(ItemID + i);
                    response.add(values[i]);
                    _LOG_V_NO_FUNC(" value[%u]=%u", i, values[i]);
                }
                _LOG_D_NO_FUNC("\n");
                //ModbusReadInputResponse(MB.Address, MB.Function, values, MB.RegisterCount);
            } else {
                response.setError(MB.Address, MB.Function, ILLEGAL_DATA_ADDRESS);
            }
            break;
        case 0x06: // (Write single register)
            //WriteItemValueResponse();
            if (ItemID) {
                OK = setItemValue(ItemID, MB.Value);
            }

            if (OK && ItemID < STATUS_STATE) write_settings();

            if (MB.Address != BROADCAST_ADR || LoadBl == 0) {
                if (!ItemID) {
                    response.setError(MB.Address, MB.Function, ILLEGAL_DATA_ADDRESS);
                } else if (!OK) {
                    response.setError(MB.Address, MB.Function, ILLEGAL_DATA_VALUE);
                } else {
                    return ECHO_RESPONSE;
                }
            }
            break;
        case 0x10: // (Write multiple register))
            //      WriteMultipleItemValueResponse();
            if (ItemID) {
                for (i = 0; i < MB.RegisterCount; i++) {
                    value = (MB.Data[i * 2] <<8) | MB.Data[(i * 2) + 1];
                    OK += setItemValue(ItemID + i, value);
                }
            }

            if (OK && ItemID < STATUS_STATE) write_settings();

            if (MB.Address != BROADCAST_ADR || LoadBl == 0) {
                if (!ItemID) {
                    response.setError(MB.Address, MB.Function, ILLEGAL_DATA_ADDRESS);
                } else if (!OK) {
                    response.setError(MB.Address, MB.Function, ILLEGAL_DATA_VALUE);
                } else  {
                    response.add(MB.Address, MB.Function, (uint16_t)MB.Register, (uint16_t)OK);
                }
            }
            break;
        default:
            break;
    }

  return response;
}

// The Node/Server receives a broadcast message from the Master
// Does not send any data back.
ModbusMessage MBbroadcast(ModbusMessage request) {
    uint8_t ItemID, i, OK = 0;
    uint16_t value;
    int16_t combined;

    ModbusDecode( (uint8_t*)request.data(), request.size());
    ItemID = mapModbusRegister2ItemID();

    if (MB.Type == MODBUS_REQUEST) {

        // Broadcast or addressed to this device
        switch (MB.Function) {
            // FC 03 and 04 are not possible with broadcast messages.
            case 0x06: // (Write single register)
                //WriteItemValueResponse();
                if (ItemID) {
                    OK = setItemValue(ItemID, MB.Value);
                }

                if (OK && ItemID < STATUS_STATE) write_settings();
                _LOG_V("Broadcast received FC06 Item:%u val:%u\n",ItemID, MB.Value);
                break;
            case 0x10: // (Write multiple register))
                // 0x0020: Balance currents
                if (MB.Register == 0x0020 && LoadBl > 1) {      // Message for Node(s)
                    Balanced[0] = (MB.Data[(LoadBl - 1) * 2] <<8) | MB.Data[(LoadBl - 1) * 2 + 1];
                    if (Balanced[0] == 0 && State == STATE_C) setState(STATE_C1);               // tell EV to stop charging if charge current is zero
                    else if ((State == STATE_B) || (State == STATE_C)) SetCurrent(Balanced[0]); // Set charge current, and PWM output
                    MainsMeter.Timeout = COMM_TIMEOUT;                          // reset 10 second timeout
                    _LOG_V("Broadcast received, Node %.1f A, MainsMeter Irms ", (float) Balanced[0]/10);

                    //now decode registers 0x0028-0x002A
                    if (MB.DataLength >= 16+6) {
                        Isum = 0;    
                        for (i=0; i<3; i++ ) {
                            combined = (MB.Data[(i * 2) + 16] <<8) + MB.Data[(i * 2) + 17]; 
                            Isum = Isum + combined;
                            MainsMeter.Irms[i] = combined;
                            _LOG_V_NO_FUNC("L%i=%.1fA,", i+1, (float)MainsMeter.Irms[i]/10);
                        }
                        _LOG_V_NO_FUNC("\n");
                    }
                } else {

                    //WriteMultipleItemValueResponse();
                    if (ItemID) {
                        for (i = 0; i < MB.RegisterCount; i++) {
                            value = (MB.Data[i * 2] <<8) | MB.Data[(i * 2) + 1];
                            OK += setItemValue(ItemID + i, value);
                        }
                    }

                    if (OK && ItemID < STATUS_STATE) write_settings();
                    _LOG_V("Other Broadcast received\n");
                }    
                break;
            default:
                break;
        }
    }

    // As it is a broadcast message, do not send response.
    return NIL_RESPONSE;              
}

// Data handler for Master
// Responses from Slaves/Nodes are handled here
void MBhandleData(ModbusMessage msg, uint32_t token) 
{
    uint8_t Address = msg.getServerID();    // returns Server ID or 0 if MM_data is shorter than 3
    if (Address == MainsMeter.Address) {
        //_LOG_A("MainsMeter data\n");
        ModbusDecode( (uint8_t*)msg.data(), msg.size());
        MainsMeter.ResponseToMeasurement();
    } else if (Address == EVMeter.Address) {
        //_LOG_A("EV Meter data\n");
        MBEVMeterResponse(msg);
    // Only responses to FC 03/04 are handled here. FC 06/10 response is only a acknowledge.
    } else {
        //_LOG_V("Received Packet with ServerID=%i, FunctionID=%i, token=%08x.\n", msg.getServerID(), msg.getFunctionCode(), token);
        ModbusDecode( (uint8_t*)msg.data(), msg.size());
        // ModbusDecode does NOT always decodes the register correctly.
        // This bug manifested itself as the <Mode=186 bug>:

        // (Timer100ms)(C1) ModbusRequest 4: Request Configuration Node 1
        // (D) (ModbusSend8)(C1) Sent packet address: 02, function: 04, reg: 0108, data: 0002.
        // (D) (ModbusDecode)(C0) Received packet (7 bytes) 02 04 04 00 00 00 0c
        // (V) (ModbusDecode)(C0)  valid Modbus packet: Address 02 Function 04 Register 0000 Response
        // (D) (receiveNodeStatus)(C0) ReceivedNode[1]Status State:0 Error:12, BalancedMax:2530, Mode:186, ConfigChanged:253.

        // The response is the response for a request of node config 0x0108, but is interpreted as a request for node status 0x0000
        //
        // Using a global variable struct ModBus MB is not a good idea, but localizing it does not
        // solve the problem.

        // Luckily we have coded the register in the token we sent....
        // token: first byte address, second byte function, third and fourth reg
        uint8_t token_function = (token & 0x00FF0000) >> 16;
        uint8_t token_address = token >> 24;
        if (token_address != MB.Address) {
            _LOG_A("ERROR: Address=%u, MB.Address=%u, token_address=%u.\n", Address, MB.Address, token_address);
        }    
        if (token_function != MB.Function) {
            _LOG_A("ERROR: MB.Function=%u, token_function=%u.\n", MB.Function, token_function);
        }    
        uint16_t reg = (token & 0x0000FFFF);
        MB.Register = reg;

        if (MB.Address > 1 && MB.Address <= NR_EVSES && (MB.Function == 03 || MB.Function == 04)) {
        
            // Packet from Node EVSE
            if (MB.Register == 0x0000) {
                // Node status
            //    _LOG_A("Node Status received\n");
                receiveNodeStatus(MB.Data, MB.Address - 1u);
            }  else if (MB.Register == 0x0108) {
                // Node EV meter settings
            //    _LOG_A("Node EV Meter settings received\n");
                receiveNodeConfig(MB.Data, MB.Address - 1u);
                return; // Do not call ModbusRequestLoop(), we still expect an Ack from the Node
            }
        }
    }
    ModbusRequestLoop();   // continue with the next request.
}


void MBhandleError(Error error, uint32_t token) 
{
  // ModbusError wraps the error code and provides a readable error message for it
  ModbusError me(error);
  uint8_t address, function;
  uint16_t reg;
  address = token >> 24;
  function = (token >> 16);
  reg = token & 0xFFFF;

  if (LoadBl == 1 && ((address>=2 && address <=8 && function == 4 && reg == 0) || address == 9)) {  //master sends out messages to nodes 2-8, if no EVSE is connected with that address
                                                                                //a timeout will be generated. This is legit!
                                                                                //same goes for broadcast address 9
    _LOG_V("Error response: %02X - %s, address: %02x, function: %02x, reg: %04x.\n", error, (const char *)me,  address, function, reg);
  }
  else {
    _LOG_A("Error response: %02X - %s, address: %02x, function: %02x, reg: %04x.\n", error, (const char *)me,  address, function, reg);
  }
  if (ModbusRequest) ModbusRequestLoop();  // continue with the next request.
}


#if SMARTEVSE_VERSION == 3
void ConfigureModbusMode(uint8_t newmode) {

    _LOG_A("changing LoadBl from %u to %u\n",LoadBl, newmode);
    
    if ((LoadBl < 2 && newmode > 1) || (LoadBl > 1 && newmode < 2) || (newmode == 255) ) {
        
        if (newmode != 255 ) LoadBl = newmode;

        // Setup Modbus workers for Node
        if (LoadBl > 1 ) {
            
            _LOG_A("Setup MBserver/Node workers, end Master/Client\n");
            // Stop Master background task (if active)
            if (newmode != 255 ) MBclient.end();    
            _LOG_A("ConfigureModbusMode1 task free ram: %u\n", uxTaskGetStackHighWaterMark( NULL ));

            // Register worker. at serverID 'LoadBl', all function codes
            MBserver.registerWorker(LoadBl, ANY_FUNCTION_CODE, &MBNodeRequest);      
            // Also add handler for all broadcast messages from Master.
            MBserver.registerWorker(BROADCAST_ADR, ANY_FUNCTION_CODE, &MBbroadcast);

            if (EVMeter.Type && EVMeter.Type != EM_API) MBserver.registerWorker(EVMeter.Address, ANY_FUNCTION_CODE, &MBEVMeterResponse);

            // Start ModbusRTU Node background task
            MBserver.begin(Serial1);

        } else if (LoadBl < 2 ) {
            // Setup Modbus workers as Master 
            // Stop Node background task (if active)
            _LOG_A("Setup Modbus as Master/Client, stop Server/Node handler\n");

            if (newmode != 255) MBserver.end();
            _LOG_A("ConfigureModbusMode2 task free ram: %u\n", uxTaskGetStackHighWaterMark( NULL ));

            MBclient.setTimeout(85);                        // Set modbus timeout to 85ms. 15ms lower then modbusRequestloop time of 100ms.
            MBclient.onDataHandler(&MBhandleData);
            MBclient.onErrorHandler(&MBhandleError);
            // Start ModbusRTU Master background task
            MBclient.begin(Serial1, 1, (uint32_t)50000U);   // pinning it to core1 reduces modbus problems. Make sure there is 50ms quiet time between messages
        } 
    } else if (newmode > 1) {
        // Register worker. at serverID 'LoadBl', all function codes
        _LOG_A("Registering new LoadBl worker at id %u\n", newmode);
        LoadBl = newmode;
        MBserver.registerWorker(newmode, ANY_FUNCTION_CODE, &MBNodeRequest);   
    }
    
}

#endif

/**
 * Validate setting ranges and dependencies
 */
void validate_settings(void) {
    uint8_t i;
    uint16_t value;

    // If value is out of range, reset it to default value
    for (i = MENU_ENTER + 1;i < MENU_EXIT; i++){
        value = getItemValue(i);
    //    _LOG_A("value %s set to %i\n",MenuStr[i].LCD, value );
        if (value > MenuStr[i].Max || value < MenuStr[i].Min) {
            value = MenuStr[i].Default;
    //        _LOG_A("set default value for %s to %i\n",MenuStr[i].LCD, value );
            setItemValue(i, value);
        }
    }

    // Sensorbox v2 has always address 0x0A
    if (MainsMeter.Type == EM_SENSORBOX) MainsMeter.Address = 0x0A;
    // set Lock variables for Solenoid or Motor
    if (Lock == 1) { lock1 = LOW; lock2 = HIGH; }                               // Solenoid
    else if (Lock == 2) { lock1 = HIGH; lock2 = LOW; }                          // Motor
    // Erase all RFID cards from ram + eeprom if set to EraseAll
    if (RFIDReader == 5) {
        DeleteAllRFID();
        setItemValue(MENU_RFIDREADER, 0);                                       // RFID Reader Disabled
    }

    // Update master node config
    if (LoadBl < 2) {
        Node[0].EVMeter = EVMeter.Type;
        Node[0].EVAddress = EVMeter.Address;
    }
          
    // Default to modbus input registers
    if (EMConfig[EM_CUSTOM].Function != 3) EMConfig[EM_CUSTOM].Function = 4;

    // Backward compatibility < 2.20
    if (EMConfig[EM_CUSTOM].IRegister == 8 || EMConfig[EM_CUSTOM].URegister == 8 || EMConfig[EM_CUSTOM].PRegister == 8 || EMConfig[EM_CUSTOM].ERegister == 8) {
        EMConfig[EM_CUSTOM].DataType = MB_DATATYPE_FLOAT32;
        EMConfig[EM_CUSTOM].IRegister = 0;
        EMConfig[EM_CUSTOM].URegister = 0;
        EMConfig[EM_CUSTOM].PRegister = 0;
        EMConfig[EM_CUSTOM].ERegister = 0;
    }

#if SMARTEVSE_VERSION == 3
    // If the address of the MainsMeter or EVmeter on a Node has changed, we must re-register the Modbus workers.
    if (LoadBl > 1) {
        if (EVMeter.Type && EVMeter.Type != EM_API) MBserver.registerWorker(EVMeter.Address, ANY_FUNCTION_CODE, &MBEVMeterResponse);
    }
#endif
    MainsMeter.Timeout = COMM_TIMEOUT;
    EVMeter.Timeout = COMM_TIMEOUT;                                             // Short Delay, to clear the error message for ~10 seconds.

}

void read_settings() {
    
    // Open preferences. true = read only,  false = read/write
    // If "settings" does not exist, it will be created, and initialized with the default values
    if (preferences.begin("settings", false) ) {                                
        bool Initialized = preferences.isKey("Config");
        Config = preferences.getUChar("Config", CONFIG); 
        Lock = preferences.getUChar("Lock", LOCK); 
        Mode = preferences.getUChar("Mode", MODE); 
        Access_bit = preferences.getUChar("Access", ACCESS_BIT);
        if (preferences.isKey("CardOffset")) {
            CardOffset = preferences.getUChar("CardOffset", CARD_OFFSET);
            //write the old 8 bits value to the new 16 bits value
            preferences.putUShort("CardOffs16", CardOffset);
            preferences.remove("CardOffset");
        }
        else
            CardOffset = preferences.getUShort("CardOffs16", CARD_OFFSET);
        LoadBl = preferences.getUChar("LoadBl", LOADBL); 
        MaxMains = preferences.getUShort("MaxMains", MAX_MAINS); 
        MaxSumMains = preferences.getUShort("MaxSumMains", MAX_SUMMAINS);
        MaxSumMainsTime = preferences.getUShort("MaxSumMainsTime", MAX_SUMMAINSTIME);
        MaxCurrent = preferences.getUShort("MaxCurrent", MAX_CURRENT); 
        MinCurrent = preferences.getUShort("MinCurrent", MIN_CURRENT); 
        MaxCircuit = preferences.getUShort("MaxCircuit", MAX_CIRCUIT); 
        Switch = preferences.getUChar("Switch", SWITCH); 
        RCmon = preferences.getUChar("RCmon", RC_MON); 
        StartCurrent = preferences.getUShort("StartCurrent", START_CURRENT); 
        StopTime = preferences.getUShort("StopTime", STOP_TIME); 
        ImportCurrent = preferences.getUShort("ImportCurrent",IMPORT_CURRENT);
        Grid = preferences.getUChar("Grid",GRID);
        SB2_WIFImode = preferences.getUChar("SB2WIFImode",SB2_WIFI_MODE);
        RFIDReader = preferences.getUChar("RFIDReader",RFID_READER);

        MainsMeter.Type = preferences.getUChar("MainsMeter", MAINS_METER);
        MainsMeter.Address = preferences.getUChar("MainsMAddress",MAINS_METER_ADDRESS);
        EVMeter.Type = preferences.getUChar("EVMeter",EV_METER);
        EVMeter.Address = preferences.getUChar("EVMeterAddress",EV_METER_ADDRESS);
        EMConfig[EM_CUSTOM].Endianness = preferences.getUChar("EMEndianness",EMCUSTOM_ENDIANESS);
        EMConfig[EM_CUSTOM].IRegister = preferences.getUShort("EMIRegister",EMCUSTOM_IREGISTER);
        EMConfig[EM_CUSTOM].IDivisor = preferences.getUChar("EMIDivisor",EMCUSTOM_IDIVISOR);
        EMConfig[EM_CUSTOM].URegister = preferences.getUShort("EMURegister",EMCUSTOM_UREGISTER);
        EMConfig[EM_CUSTOM].UDivisor = preferences.getUChar("EMUDivisor",EMCUSTOM_UDIVISOR);
        EMConfig[EM_CUSTOM].PRegister = preferences.getUShort("EMPRegister",EMCUSTOM_PREGISTER);
        EMConfig[EM_CUSTOM].PDivisor = preferences.getUChar("EMPDivisor",EMCUSTOM_PDIVISOR);
        EMConfig[EM_CUSTOM].ERegister = preferences.getUShort("EMERegister",EMCUSTOM_EREGISTER);
        EMConfig[EM_CUSTOM].EDivisor = preferences.getUChar("EMEDivisor",EMCUSTOM_EDIVISOR);
        EMConfig[EM_CUSTOM].DataType = (mb_datatype)preferences.getUChar("EMDataType",EMCUSTOM_DATATYPE);
        EMConfig[EM_CUSTOM].Function = preferences.getUChar("EMFunction",EMCUSTOM_FUNCTION);
        WIFImode = preferences.getUChar("WIFImode",WIFI_MODE);
        DelayedStartTime.epoch2 = preferences.getULong("DelayedStartTim", DELAYEDSTARTTIME); //epoch2 is 4 bytes long on arduino; NVS key has reached max size
        DelayedStopTime.epoch2 = preferences.getULong("DelayedStopTime", DELAYEDSTOPTIME);    //epoch2 is 4 bytes long on arduino
        DelayedRepeat = preferences.getUShort("DelayedRepeat", 0);
        LCDlock = preferences.getUChar("LCDlock", LCD_LOCK);
        AutoUpdate = preferences.getUChar("AutoUpdate", AUTOUPDATE);


        EnableC2 = (EnableC2_t) preferences.getUShort("EnableC2", ENABLE_C2);
        strncpy(RequiredEVCCID, preferences.getString("RequiredEVCCID", "").c_str(), sizeof(RequiredEVCCID));
        maxTemp = preferences.getUShort("maxTemp", MAX_TEMPERATURE);

#if ENABLE_OCPP
        OcppMode = preferences.getUChar("OcppMode", OCPP_MODE);
#endif //ENABLE_OCPP

        preferences.end();                                  

        // Store settings when not initialized
        if (!Initialized) write_settings();

    } else {
        _LOG_A("Can not open preferences!\n");
    }
}

void write_settings(void) {

    validate_settings();

 if (preferences.begin("settings", false) ) {

    preferences.putUChar("Config", Config); 
    preferences.putUChar("Lock", Lock); 
    preferences.putUChar("Mode", Mode); 
    preferences.putUChar("Access", Access_bit);
    preferences.putUShort("CardOffs16", CardOffset);
    preferences.putUChar("LoadBl", LoadBl); 
    preferences.putUShort("MaxMains", MaxMains); 
    preferences.putUShort("MaxSumMains", MaxSumMains);
    preferences.putUShort("MaxSumMainsTime", MaxSumMainsTime);
    preferences.putUShort("MaxCurrent", MaxCurrent); 
    preferences.putUShort("MinCurrent", MinCurrent); 
    preferences.putUShort("MaxCircuit", MaxCircuit); 
    preferences.putUChar("Switch", Switch); 
    preferences.putUChar("RCmon", RCmon); 
    preferences.putUShort("StartCurrent", StartCurrent); 
    preferences.putUShort("StopTime", StopTime); 
    preferences.putUShort("ImportCurrent", ImportCurrent);
    preferences.putUChar("Grid", Grid);
    preferences.putUChar("SB2WIFImode", SB2_WIFImode);
    preferences.putUChar("RFIDReader", RFIDReader);

    preferences.putUChar("MainsMeter", MainsMeter.Type);
    preferences.putUChar("MainsMAddress", MainsMeter.Address);
    preferences.putUChar("EVMeter", EVMeter.Type);
    preferences.putUChar("EVMeterAddress", EVMeter.Address);
    preferences.putUChar("EMEndianness", EMConfig[EM_CUSTOM].Endianness);
    preferences.putUShort("EMIRegister", EMConfig[EM_CUSTOM].IRegister);
    preferences.putUChar("EMIDivisor", EMConfig[EM_CUSTOM].IDivisor);
    preferences.putUShort("EMURegister", EMConfig[EM_CUSTOM].URegister);
    preferences.putUChar("EMUDivisor", EMConfig[EM_CUSTOM].UDivisor);
    preferences.putUShort("EMPRegister", EMConfig[EM_CUSTOM].PRegister);
    preferences.putUChar("EMPDivisor", EMConfig[EM_CUSTOM].PDivisor);
    preferences.putUShort("EMERegister", EMConfig[EM_CUSTOM].ERegister);
    preferences.putUChar("EMEDivisor", EMConfig[EM_CUSTOM].EDivisor);
    preferences.putUChar("EMDataType", EMConfig[EM_CUSTOM].DataType);
    preferences.putUChar("EMFunction", EMConfig[EM_CUSTOM].Function);
    preferences.putUChar("WIFImode", WIFImode);
    preferences.putUShort("EnableC2", EnableC2);
    preferences.putString("RequiredEVCCID", String(RequiredEVCCID));
    preferences.putUShort("maxTemp", maxTemp);
    preferences.putUChar("AutoUpdate", AutoUpdate);
    preferences.putUChar("LCDlock", LCDlock);

#if ENABLE_OCPP
    preferences.putUChar("OcppMode", OcppMode);
#endif //ENABLE_OCPP

    preferences.end();

    _LOG_I("settings saved\n");

 } else {
     _LOG_A("Can not open preferences!\n");
 }


    if (LoadBl == 1) {                                                          // Master mode
        uint16_t i, values[MODBUS_SYS_CONFIG_COUNT];
        for (i = 0; i < MODBUS_SYS_CONFIG_COUNT; i++) {
            values[i] = getItemValue(MENU_MODE + i);
        }
        // Broadcast settings to other controllers
        ModbusWriteMultipleRequest(BROADCAST_ADR, MODBUS_SYS_CONFIG_START, values, MODBUS_SYS_CONFIG_COUNT);
    }

    ConfigChanged = 1;
}


/* Takes TimeString in format
 * String = "2023-04-14T11:31"
 * and store it in the DelayedTimeStruct
 * returns 0 on success, 1 on failure
*/
int StoreTimeString(String DelayedTimeStr, DelayedTimeStruct *DelayedTime) {
    // Parse the time string
    tm delayedtime_tm = {};
    if (strptime(DelayedTimeStr.c_str(), "%Y-%m-%dT%H:%M", &delayedtime_tm)) {
        delayedtime_tm.tm_isdst = -1;                 //so mktime is going to figure out whether DST is there or not
        DelayedTime->epoch2 = mktime(&delayedtime_tm) - EPOCH2_OFFSET;
        // Compare the times
        time_t now = time(nullptr);             //get current local time
        DelayedTime->diff = DelayedTime->epoch2 - (mktime(localtime(&now)) - EPOCH2_OFFSET);
        return 0;
    }
    //error TODO not sure whether we keep the old time or reset it to zero?
    //DelayedTime.epoch2 = 0;
    //DelayedTime.diff = 0;
    return 1;
}

//make mongoose 7.14 compatible with 7.13
#define mg_http_match_uri(X,Y) mg_match(X->uri, mg_str(Y), NULL)

// handles URI, returns true if handled, false if not
bool handle_URI(struct mg_connection *c, struct mg_http_message *hm,  webServerRequest* request) {
//    if (mg_match(hm->uri, mg_str("/settings"), NULL)) {               // REST API call?
    if (mg_http_match_uri(hm, "/settings")) {                            // REST API call?
      if (!memcmp("GET", hm->method.buf, hm->method.len)) {                     // if GET
        String mode = "N/A";
        int modeId = -1;
        if(Access_bit == 0)  {
            mode = "OFF";
            modeId=0;
        } else {
            switch(Mode) {
                case MODE_NORMAL: mode = "NORMAL"; modeId=1; break;
                case MODE_SOLAR: mode = "SOLAR"; modeId=2; break;
                case MODE_SMART: mode = "SMART"; modeId=3; break;
            }
        }
        String backlight = "N/A";
        switch(BacklightSet) {
            case 0: backlight = "OFF"; break;
            case 1: backlight = "ON"; break;
            case 2: backlight = "DIMMED"; break;
        }
        String evstate = StrStateNameWeb[State];
        String error = getErrorNameWeb(ErrorFlags);
        int errorId = getErrorId(ErrorFlags);

        if (ErrorFlags & NO_SUN) {
            evstate += " - " + error;
            error = "None";
            errorId = 0;
        }

        boolean evConnected = pilot != PILOT_12V;                    //when access bit = 1, p.ex. in OFF mode, the STATEs are no longer updated

        DynamicJsonDocument doc(3072); // https://arduinojson.org/v6/assistant/
        doc["version"] = String(VERSION);
        doc["serialnr"] = serialnr;
        doc["mode"] = mode;
        doc["mode_id"] = modeId;
        doc["car_connected"] = evConnected;

        if(WiFi.isConnected()) {
            switch(WiFi.status()) {
                case WL_NO_SHIELD:          doc["wifi"]["status"] = "WL_NO_SHIELD"; break;
                case WL_IDLE_STATUS:        doc["wifi"]["status"] = "WL_IDLE_STATUS"; break;
                case WL_NO_SSID_AVAIL:      doc["wifi"]["status"] = "WL_NO_SSID_AVAIL"; break;
                case WL_SCAN_COMPLETED:     doc["wifi"]["status"] = "WL_SCAN_COMPLETED"; break;
                case WL_CONNECTED:          doc["wifi"]["status"] = "WL_CONNECTED"; break;
                case WL_CONNECT_FAILED:     doc["wifi"]["status"] = "WL_CONNECT_FAILED"; break;
                case WL_CONNECTION_LOST:    doc["wifi"]["status"] = "WL_CONNECTION_LOST"; break;
                case WL_DISCONNECTED:       doc["wifi"]["status"] = "WL_DISCONNECTED"; break;
                default:                    doc["wifi"]["status"] = "UNKNOWN"; break;
            }

            doc["wifi"]["ssid"] = WiFi.SSID();    
            doc["wifi"]["rssi"] = WiFi.RSSI();    
            doc["wifi"]["bssid"] = WiFi.BSSIDstr();  
        }
        
        doc["evse"]["temp"] = TempEVSE;
        doc["evse"]["temp_max"] = maxTemp;
        doc["evse"]["connected"] = evConnected;
        doc["evse"]["access"] = Access_bit == 1;
        doc["evse"]["mode"] = Mode;
        doc["evse"]["loadbl"] = LoadBl;
        doc["evse"]["pwm"] = CurrentPWM;
        doc["evse"]["custombutton"] = CustomButton;
        doc["evse"]["solar_stop_timer"] = SolarStopTimer;
        doc["evse"]["state"] = evstate;
        doc["evse"]["state_id"] = State;
        doc["evse"]["error"] = error;
        doc["evse"]["error_id"] = errorId;
        doc["evse"]["rfid"] = !RFIDReader ? "Not Installed" : RFIDstatus >= 8 ? "NOSTATUS" : StrRFIDStatusWeb[RFIDstatus];
        if (RFIDReader && RFIDReader != 6) { //RFIDLastRead not updated in Remote/OCPP mode
            char buf[15];
            if (RFID[0] == 0x01) {  // old reader 6 byte UID starts at RFID[1]
                sprintf(buf, "%02X%02X%02X%02X%02X%02X", RFID[1], RFID[2], RFID[3], RFID[4], RFID[5], RFID[6]);
            } else {
                sprintf(buf, "%02X%02X%02X%02X%02X%02X%02X", RFID[0], RFID[1], RFID[2], RFID[3], RFID[4], RFID[5], RFID[6]);
            }
            doc["evse"]["rfid_lastread"] = buf;
        }

        doc["settings"]["charge_current"] = Balanced[0];
        doc["settings"]["override_current"] = OverrideCurrent;
        doc["settings"]["current_min"] = MinCurrent;
        doc["settings"]["current_max"] = MaxCurrent;
        doc["settings"]["current_main"] = MaxMains;
        doc["settings"]["current_max_circuit"] = MaxCircuit;
        doc["settings"]["current_max_sum_mains"] = MaxSumMains;
        doc["settings"]["max_sum_mains_time"] = MaxSumMainsTime;
        doc["settings"]["solar_max_import"] = ImportCurrent;
        doc["settings"]["solar_start_current"] = StartCurrent;
        doc["settings"]["solar_stop_time"] = StopTime;
        doc["settings"]["enable_C2"] = StrEnableC2[EnableC2];
        doc["settings"]["mains_meter"] = EMConfig[MainsMeter.Type].Desc;
        doc["settings"]["starttime"] = (DelayedStartTime.epoch2 ? DelayedStartTime.epoch2 + EPOCH2_OFFSET : 0);
        doc["settings"]["stoptime"] = (DelayedStopTime.epoch2 ? DelayedStopTime.epoch2 + EPOCH2_OFFSET : 0);
        doc["settings"]["repeat"] = DelayedRepeat;
        doc["settings"]["lcdlock"] = LCDlock;
#if MODEM
            doc["settings"]["required_evccid"] = RequiredEVCCID;
            doc["settings"]["modem"] = "Experiment";

            doc["ev_state"]["initial_soc"] = InitialSoC;
            doc["ev_state"]["remaining_soc"] = RemainingSoC;
            doc["ev_state"]["full_soc"] = FullSoC;
            doc["ev_state"]["energy_capacity"] = EnergyCapacity > 0 ? round((float)EnergyCapacity / 100)/10 : -1; //in kWh, precision 1 decimal;
            doc["ev_state"]["energy_request"] = EnergyRequest > 0 ? round((float)EnergyRequest / 100)/10 : -1; //in kWh, precision 1 decimal
            doc["ev_state"]["computed_soc"] = ComputedSoC;
            doc["ev_state"]["evccid"] = EVCCID;
            doc["ev_state"]["time_until_full"] = TimeUntilFull;
#endif

#if MQTT
        doc["mqtt"]["host"] = MQTTHost;
        doc["mqtt"]["port"] = MQTTPort;
        doc["mqtt"]["topic_prefix"] = MQTTprefix;
        doc["mqtt"]["username"] = MQTTuser;
        doc["mqtt"]["password_set"] = MQTTpassword != "";

        if (MQTTclient.connected) {
            doc["mqtt"]["status"] = "Connected";
        } else {
            doc["mqtt"]["status"] = "Disconnected";
        }
#endif

#if ENABLE_OCPP
        doc["ocpp"]["mode"] = OcppMode ? "Enabled" : "Disabled";
        doc["ocpp"]["backend_url"] = OcppWsClient ? OcppWsClient->getBackendUrl() : "";
        doc["ocpp"]["cb_id"] = OcppWsClient ? OcppWsClient->getChargeBoxId() : "";
        doc["ocpp"]["auth_key"] = OcppWsClient ? OcppWsClient->getAuthKey() : "";

        {
            auto freevendMode = MicroOcpp::getConfigurationPublic(MO_CONFIG_EXT_PREFIX "FreeVendActive");
            doc["ocpp"]["auto_auth"] = freevendMode && freevendMode->getBool() ? "Enabled" : "Disabled";
            auto freevendIdTag = MicroOcpp::getConfigurationPublic(MO_CONFIG_EXT_PREFIX "FreeVendIdTag");
            doc["ocpp"]["auto_auth_idtag"] = freevendIdTag ? freevendIdTag->getString() : "";
        }

        if (OcppWsClient && OcppWsClient->isConnected()) {
            doc["ocpp"]["status"] = "Connected";
        } else {
            doc["ocpp"]["status"] = "Disconnected";
        }
#endif //ENABLE_OCPP

        doc["home_battery"]["current"] = homeBatteryCurrent;
        doc["home_battery"]["last_update"] = homeBatteryLastUpdate;

        doc["ev_meter"]["description"] = EMConfig[EVMeter.Type].Desc;
        doc["ev_meter"]["address"] = EVMeter.Address;
        doc["ev_meter"]["import_active_power"] = round((float)EVMeter.PowerMeasured / 100)/10; //in kW, precision 1 decimal
        doc["ev_meter"]["total_kwh"] = round((float)EVMeter.Energy / 100)/10; //in kWh, precision 1 decimal
        doc["ev_meter"]["charged_kwh"] = round((float)EVMeter.EnergyCharged / 100)/10; //in kWh, precision 1 decimal
        doc["ev_meter"]["currents"]["TOTAL"] = EVMeter.Irms[0] + EVMeter.Irms[1] + EVMeter.Irms[2];
        doc["ev_meter"]["currents"]["L1"] = EVMeter.Irms[0];
        doc["ev_meter"]["currents"]["L2"] = EVMeter.Irms[1];
        doc["ev_meter"]["currents"]["L3"] = EVMeter.Irms[2];
        doc["ev_meter"]["import_active_energy"] = round((float)EVMeter.Import_active_energy / 100)/10; //in kWh, precision 1 decimal
        doc["ev_meter"]["export_active_energy"] = round((float)EVMeter.Export_active_energy / 100)/10; //in kWh, precision 1 decimal

        doc["mains_meter"]["import_active_energy"] = round((float)MainsMeter.Import_active_energy / 100)/10; //in kWh, precision 1 decimal
        doc["mains_meter"]["export_active_energy"] = round((float)MainsMeter.Export_active_energy / 100)/10; //in kWh, precision 1 decimal

        doc["phase_currents"]["TOTAL"] = MainsMeter.Irms[0] + MainsMeter.Irms[1] + MainsMeter.Irms[2];
        doc["phase_currents"]["L1"] = MainsMeter.Irms[0];
        doc["phase_currents"]["L2"] = MainsMeter.Irms[1];
        doc["phase_currents"]["L3"] = MainsMeter.Irms[2];
        doc["phase_currents"]["last_data_update"] = phasesLastUpdate;
        doc["phase_currents"]["original_data"]["TOTAL"] = IrmsOriginal[0] + IrmsOriginal[1] + IrmsOriginal[2];
        doc["phase_currents"]["original_data"]["L1"] = IrmsOriginal[0];
        doc["phase_currents"]["original_data"]["L2"] = IrmsOriginal[1];
        doc["phase_currents"]["original_data"]["L3"] = IrmsOriginal[2];
        
        doc["backlight"]["timer"] = BacklightTimer;
        doc["backlight"]["status"] = backlight;

        doc["color"]["off"]["R"] = ColorOff[0];
        doc["color"]["off"]["G"] = ColorOff[1];
        doc["color"]["off"]["B"] = ColorOff[2];
        doc["color"]["normal"]["R"] = ColorNormal[0];
        doc["color"]["normal"]["G"] = ColorNormal[1];
        doc["color"]["normal"]["B"] = ColorNormal[2];
        doc["color"]["smart"]["R"] = ColorSmart[0];
        doc["color"]["smart"]["G"] = ColorSmart[1];
        doc["color"]["smart"]["B"] = ColorSmart[2];
        doc["color"]["solar"]["R"] = ColorSolar[0];
        doc["color"]["solar"]["G"] = ColorSolar[1];
        doc["color"]["solar"]["B"] = ColorSolar[2];
        doc["color"]["custom"]["R"] = ColorCustom[0];
        doc["color"]["custom"]["G"] = ColorCustom[1];
        doc["color"]["custom"]["B"] = ColorCustom[2];

        String json;
        serializeJson(doc, json);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\n", json.c_str());    // Yes. Respond JSON
        return true;
      } else if (!memcmp("POST", hm->method.buf, hm->method.len)) {                     // if POST
        if(request->hasParam("mqtt_update")) {
            return false;                                                       // handled in network.cpp
        }
        DynamicJsonDocument doc(512); // https://arduinojson.org/v6/assistant/

        if(request->hasParam("backlight")) {
            int backlight = request->getParam("backlight")->value().toInt();
            BacklightTimer = backlight * BACKLIGHT;
            doc["Backlight"] = backlight;
        }

        if(request->hasParam("current_min")) {
            int current = request->getParam("current_min")->value().toInt();
            if(current >= MIN_CURRENT && current <= 16 && LoadBl < 2) {
                MinCurrent = current;
                doc["current_min"] = MinCurrent;
                write_settings();
            } else {
                doc["current_min"] = "Value not allowed!";
            }
        }

        if(request->hasParam("current_max_sum_mains")) {
            int current = request->getParam("current_max_sum_mains")->value().toInt();
            if((current == 0 || (current >= 10 && current <= 600)) && LoadBl < 2) {
                MaxSumMains = current;
                doc["current_max_sum_mains"] = MaxSumMains;
                write_settings();
            } else {
                doc["current_max_sum_mains"] = "Value not allowed!";
            }
        }

        if(request->hasParam("max_sum_mains_timer")) {
            int time = request->getParam("max_sum_mains_timer")->value().toInt();
            if(time >= 0 && time <= 60 && LoadBl < 2) {
                MaxSumMainsTime = time;
                doc["max_sum_mains_time"] = MaxSumMainsTime;
                write_settings();
            } else {
                doc["max_sum_mains_time"] = "Value not allowed!";
            }
        }

        if(request->hasParam("disable_override_current")) {
            OverrideCurrent = 0;
            doc["disable_override_current"] = "OK";
        }

        if(request->hasParam("custombutton")) {
            CustomButton = request->getParam("custombutton")->value().toInt() > 0;
            doc["custombutton"] = CustomButton;
        }

        if(request->hasParam("mode")) {
            String mode = request->getParam("mode")->value();

            //first check if we have a delayed mode switch
            if(request->hasParam("starttime")) {
                String DelayedStartTimeStr = request->getParam("starttime")->value();
                //string time_str = "2023-04-14T11:31";
                if (!StoreTimeString(DelayedStartTimeStr, &DelayedStartTime)) {
                    //parse OK
                    if (DelayedStartTime.diff > 0)
                        setAccess(0);                         //switch to OFF, we are Delayed Charging
                    else {//we are in the past so no delayed charging
                        DelayedStartTime.epoch2 = DELAYEDSTARTTIME;
                        DelayedStopTime.epoch2 = DELAYEDSTOPTIME;
                        DelayedRepeat = 0;
                    }
                }
                else {
                    //we couldn't parse the string, so we are NOT Delayed Charging
                    DelayedStartTime.epoch2 = DELAYEDSTARTTIME;
                    DelayedStopTime.epoch2 = DELAYEDSTOPTIME;
                    DelayedRepeat = 0;
                }

                // so now we might have a starttime and we might be Delayed Charging
                if (DelayedStartTime.epoch2) {
                    //we only accept a DelayedStopTime if we have a valid DelayedStartTime
                    if(request->hasParam("stoptime")) {
                        String DelayedStopTimeStr = request->getParam("stoptime")->value();
                        //string time_str = "2023-04-14T11:31";
                        if (!StoreTimeString(DelayedStopTimeStr, &DelayedStopTime)) {
                            //parse OK
                            if (DelayedStopTime.diff <= 0 || DelayedStopTime.epoch2 <= DelayedStartTime.epoch2)
                                //we are in the past or DelayedStopTime before DelayedStartTime so no DelayedStopTime
                                DelayedStopTime.epoch2 = DELAYEDSTOPTIME;
                        }
                        else
                            //we couldn't parse the string, so no DelayedStopTime
                            DelayedStopTime.epoch2 = DELAYEDSTOPTIME;
                        doc["stoptime"] = (DelayedStopTime.epoch2 ? DelayedStopTime.epoch2 + EPOCH2_OFFSET : 0);
                        if(request->hasParam("repeat")) {
                            int Repeat = request->getParam("repeat")->value().toInt();
                            if (Repeat >= 0 && Repeat <= 1) {                                   //boundary check
                                DelayedRepeat = Repeat;
                                doc["repeat"] = Repeat;
                            }
                        }
                    }

                }
                doc["starttime"] = (DelayedStartTime.epoch2 ? DelayedStartTime.epoch2 + EPOCH2_OFFSET : 0);
            } else
                DelayedStartTime.epoch2 = DELAYEDSTARTTIME;


            switch(mode.toInt()) {
                case 0: // OFF
                    ToModemWaitStateTimer = 0;
                    ToModemDoneStateTimer = 0;
                    LeaveModemDoneStateTimer = 0;
                    LeaveModemDeniedStateTimer = 0;
                    setAccess(0);
                    break;
                case 1:
                    setMode(MODE_NORMAL);
                    break;
                case 2:
                    setMode(MODE_SOLAR);
                    break;
                case 3:
                    setMode(MODE_SMART);
                    break;
                default:
                    mode = "Value not allowed!";
            }
            doc["mode"] = mode;
        }

        if(request->hasParam("enable_C2")) {
            EnableC2 = (EnableC2_t) request->getParam("enable_C2")->value().toInt();
            write_settings();
            doc["settings"]["enable_C2"] = StrEnableC2[EnableC2];
        }

        if(request->hasParam("stop_timer")) {
            int stop_timer = request->getParam("stop_timer")->value().toInt();

            if(stop_timer >= 0 && stop_timer <= 60) {
                StopTime = stop_timer;
                doc["stop_timer"] = true;
                write_settings();
            } else {
                doc["stop_timer"] = false;
            }

        }

        if(Mode == MODE_NORMAL || Mode == MODE_SMART) {
            if(request->hasParam("override_current")) {
                int current = request->getParam("override_current")->value().toInt();
                if (LoadBl < 2 && (current == 0 || (current >= ( MinCurrent * 10 ) && current <= ( MaxCurrent * 10 )))) { //OverrideCurrent not possible on Slave
                    OverrideCurrent = current;
                    doc["override_current"] = OverrideCurrent;
                } else {
                    doc["override_current"] = "Value not allowed!";
                }
            }
        }

        if(request->hasParam("solar_start_current")) {
            int current = request->getParam("solar_start_current")->value().toInt();
            if(current >= 0 && current <= 48) {
                StartCurrent = current;
                doc["solar_start_current"] = StartCurrent;
                write_settings();
            } else {
                doc["solar_start_current"] = "Value not allowed!";
            }
        }

        if(request->hasParam("solar_max_import")) {
            int current = request->getParam("solar_max_import")->value().toInt();
            if(current >= 0 && current <= 48) {
                ImportCurrent = current;
                doc["solar_max_import"] = ImportCurrent;
                write_settings();
            } else {
                doc["solar_max_import"] = "Value not allowed!";
            }
        }

        //special section to post stuff for experimenting with an ISO15118 modem
        if(request->hasParam("override_pwm")) {
            int pwm = request->getParam("override_pwm")->value().toInt();
            if (pwm == 0){
                CP_OFF;
                CPDutyOverride = true;
            } else if (pwm < 0){
                CP_ON;
                CPDutyOverride = false;
                pwm = 100; // 10% until next loop, to be safe, corresponds to 6A
            } else{
                CP_ON;
                CPDutyOverride = true;
            }

            SetCPDuty(pwm);
            doc["override_pwm"] = pwm;
        }

        //allow basic plug 'n charge based on evccid
        //if required_evccid is set to a value, SmartEVSE will only allow charging requests from said EVCCID
        if(request->hasParam("required_evccid")) {
            if (request->getParam("required_evccid")->value().length() <= 32) {
                strncpy(RequiredEVCCID, request->getParam("required_evccid")->value().c_str(), sizeof(RequiredEVCCID));
                doc["required_evccid"] = RequiredEVCCID;
                write_settings();
            } else {
                doc["required_evccid"] = "EVCCID too long (max 32 char)";
            }
        }

        if(request->hasParam("lcdlock")) {
            int lock = request->getParam("lcdlock")->value().toInt();
            if (lock >= 0 && lock <= 1) {                                   //boundary check
                LCDlock = lock;
                doc["lcdlock"] = lock;
                write_settings();
            }
        }

#if ENABLE_OCPP
        if(request->hasParam("ocpp_update")) {
            if (request->getParam("ocpp_update")->value().toInt() == 1) {

                if(request->hasParam("ocpp_mode")) {
                    OcppMode = request->getParam("ocpp_mode")->value().toInt();
                    doc["ocpp_mode"] = OcppMode;
                }

                if(request->hasParam("ocpp_backend_url")) {
                    if (OcppWsClient) {
                        OcppWsClient->setBackendUrl(request->getParam("ocpp_backend_url")->value().c_str());
                        doc["ocpp_backend_url"] = OcppWsClient->getBackendUrl();
                    } else {
                        doc["ocpp_backend_url"] = "Can only update when OCPP enabled";
                    }
                }

                if(request->hasParam("ocpp_cb_id")) {
                    if (OcppWsClient) {
                        OcppWsClient->setChargeBoxId(request->getParam("ocpp_cb_id")->value().c_str());
                        doc["ocpp_cb_id"] = OcppWsClient->getChargeBoxId();
                    } else {
                        doc["ocpp_cb_id"] = "Can only update when OCPP enabled";
                    }
                }

                if(request->hasParam("ocpp_auth_key")) {
                    if (OcppWsClient) {
                        OcppWsClient->setAuthKey(request->getParam("ocpp_auth_key")->value().c_str());
                        doc["ocpp_auth_key"] = OcppWsClient->getAuthKey();
                    } else {
                        doc["ocpp_auth_key"] = "Can only update when OCPP enabled";
                    }
                }

                if(request->hasParam("ocpp_auto_auth")) {
                    auto freevendMode = MicroOcpp::getConfigurationPublic(MO_CONFIG_EXT_PREFIX "FreeVendActive");
                    if (freevendMode) {
                        freevendMode->setBool(request->getParam("ocpp_auto_auth")->value().toInt());
                        doc["ocpp_auto_auth"] = freevendMode->getBool() ? 1 : 0;
                    } else {
                        doc["ocpp_auto_auth"] = "Can only update when OCPP enabled";
                    }
                }

                if(request->hasParam("ocpp_auto_auth_idtag")) {
                    auto freevendIdTag = MicroOcpp::getConfigurationPublic(MO_CONFIG_EXT_PREFIX "FreeVendIdTag");
                    if (freevendIdTag) {
                        freevendIdTag->setString(request->getParam("ocpp_auto_auth_idtag")->value().c_str());
                        doc["ocpp_auto_auth_idtag"] = freevendIdTag->getString();
                    } else {
                        doc["ocpp_auto_auth_idtag"] = "Can only update when OCPP enabled";
                    }
                }

                // Apply changes in OcppWsClient
                if (OcppWsClient) {
                    OcppWsClient->reloadConfigs();
                }
                MicroOcpp::configuration_save();
                write_settings();
            }
        }
#endif //ENABLE_OCPP

        String json;
        serializeJson(doc, json);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\n", json.c_str());    // Yes. Respond JSON
        return true;
      }
    } else if (mg_http_match_uri(hm, "/color_off") && !memcmp("POST", hm->method.buf, hm->method.len)) {
        DynamicJsonDocument doc(200);
        
        if (request->hasParam("R") && request->hasParam("G") && request->hasParam("B")) {
            int32_t R = request->getParam("R")->value().toInt();
            int32_t G = request->getParam("G")->value().toInt();
            int32_t B = request->getParam("B")->value().toInt();

            // R,G,B is between 0..255
            if ((R >= 0 && R < 256) && (G >= 0 && G < 256) && (B >= 0 && B < 256)) {
                ColorOff[0] = R;
                ColorOff[1] = G;
                ColorOff[2] = B;
                doc["color"]["off"]["R"] = ColorOff[0];
                doc["color"]["off"]["G"] = ColorOff[1];
                doc["color"]["off"]["B"] = ColorOff[2];
            }
        }

        String json;
        serializeJson(doc, json);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\r\n", json.c_str());    // Yes. Respond JSON
        return true;
    } else if (mg_http_match_uri(hm, "/color_normal") && !memcmp("POST", hm->method.buf, hm->method.len)) {
        DynamicJsonDocument doc(200);
        
        if (request->hasParam("R") && request->hasParam("G") && request->hasParam("B")) {
            int32_t R = request->getParam("R")->value().toInt();
            int32_t G = request->getParam("G")->value().toInt();
            int32_t B = request->getParam("B")->value().toInt();

            // R,G,B is between 0..255
            if ((R >= 0 && R < 256) && (G >= 0 && G < 256) && (B >= 0 && B < 256)) {
                ColorNormal[0] = R;
                ColorNormal[1] = G;
                ColorNormal[2] = B;
                doc["color"]["normal"]["R"] = ColorNormal[0];
                doc["color"]["normal"]["G"] = ColorNormal[1];
                doc["color"]["normal"]["B"] = ColorNormal[2];
            }
        }

        String json;
        serializeJson(doc, json);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\r\n", json.c_str());    // Yes. Respond JSON
        return true;
    } else if (mg_http_match_uri(hm, "/color_smart") && !memcmp("POST", hm->method.buf, hm->method.len)) {
        DynamicJsonDocument doc(200);
        
        if (request->hasParam("R") && request->hasParam("G") && request->hasParam("B")) {
            int32_t R = request->getParam("R")->value().toInt();
            int32_t G = request->getParam("G")->value().toInt();
            int32_t B = request->getParam("B")->value().toInt();

            // R,G,B is between 0..255
            if ((R >= 0 && R < 256) && (G >= 0 && G < 256) && (B >= 0 && B < 256)) {
                ColorSmart[0] = R;
                ColorSmart[1] = G;
                ColorSmart[2] = B;
                doc["color"]["smart"]["R"] = ColorSmart[0];
                doc["color"]["smart"]["G"] = ColorSmart[1];
                doc["color"]["smart"]["B"] = ColorSmart[2];
            }
        }

        String json;
        serializeJson(doc, json);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\r\n", json.c_str());    // Yes. Respond JSON
        return true;
    } else if (mg_http_match_uri(hm, "/color_solar") && !memcmp("POST", hm->method.buf, hm->method.len)) {
        DynamicJsonDocument doc(200);
        
        if (request->hasParam("R") && request->hasParam("G") && request->hasParam("B")) {
            int32_t R = request->getParam("R")->value().toInt();
            int32_t G = request->getParam("G")->value().toInt();
            int32_t B = request->getParam("B")->value().toInt();

            // R,G,B is between 0..255
            if ((R >= 0 && R < 256) && (G >= 0 && G < 256) && (B >= 0 && B < 256)) {
                ColorSolar[0] = R;
                ColorSolar[1] = G;
                ColorSolar[2] = B;
                doc["color"]["solar"]["R"] = ColorSolar[0];
                doc["color"]["solar"]["G"] = ColorSolar[1];
                doc["color"]["solar"]["B"] = ColorSolar[2];
            }
        }

        String json;
        serializeJson(doc, json);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\r\n", json.c_str());    // Yes. Respond JSON
        return true;
    } else if (mg_http_match_uri(hm, "/color_custom") && !memcmp("POST", hm->method.buf, hm->method.len)) {
        DynamicJsonDocument doc(200);
        
        if (request->hasParam("R") && request->hasParam("G") && request->hasParam("B")) {
            int32_t R = request->getParam("R")->value().toInt();
            int32_t G = request->getParam("G")->value().toInt();
            int32_t B = request->getParam("B")->value().toInt();

            // R,G,B is between 0..255
            if ((R >= 0 && R < 256) && (G >= 0 && G < 256) && (B >= 0 && B < 256)) {
                ColorCustom[0] = R;
                ColorCustom[1] = G;
                ColorCustom[2] = B;
                doc["color"]["custom"]["R"] = ColorCustom[0];
                doc["color"]["custom"]["G"] = ColorCustom[1];
                doc["color"]["custom"]["B"] = ColorCustom[2];
            }
        }

        String json;
        serializeJson(doc, json);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\r\n", json.c_str());    // Yes. Respond JSON
        return true;
    } else if (mg_http_match_uri(hm, "/currents") && !memcmp("POST", hm->method.buf, hm->method.len)) {
        DynamicJsonDocument doc(200);

        if(request->hasParam("battery_current")) {
            if (LoadBl < 2) {
                homeBatteryCurrent = request->getParam("battery_current")->value().toInt();
                homeBatteryLastUpdate = time(NULL);
                doc["battery_current"] = homeBatteryCurrent;
            } else
                doc["battery_current"] = "not allowed on slave";
        }

        if(MainsMeter.Type == EM_API) {
            if(request->hasParam("L1") && request->hasParam("L2") && request->hasParam("L3")) {
                if (LoadBl < 2) {
                    MainsMeter.Irms[0] = request->getParam("L1")->value().toInt();
                    MainsMeter.Irms[1] = request->getParam("L2")->value().toInt();
                    MainsMeter.Irms[2] = request->getParam("L3")->value().toInt();

                    CalcIsum();
                    for (int x = 0; x < 3; x++) {
                        doc["original"]["L" + x] = IrmsOriginal[x];
                        doc["L" + x] = MainsMeter.Irms[x];
                    }
                    doc["TOTAL"] = Isum;

                    MainsMeter.Timeout = COMM_TIMEOUT;

                } else
                    doc["TOTAL"] = "not allowed on slave";
            }
        }

        String json;
        serializeJson(doc, json);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\r\n", json.c_str());    // Yes. Respond JSON
        return true;
    } else if (mg_http_match_uri(hm, "/ev_meter") && !memcmp("POST", hm->method.buf, hm->method.len)) {
        DynamicJsonDocument doc(200);

        if(EVMeter.Type == EM_API) {
            if(request->hasParam("L1") && request->hasParam("L2") && request->hasParam("L3")) {

                EVMeter.Irms[0] = request->getParam("L1")->value().toInt();
                EVMeter.Irms[1] = request->getParam("L2")->value().toInt();
                EVMeter.Irms[2] = request->getParam("L3")->value().toInt();
                EVMeter.CalcImeasured();
                EVMeter.Timeout = COMM_EVTIMEOUT;
                for (int x = 0; x < 3; x++)
                    doc["ev_meter"]["currents"]["L" + x] = EVMeter.Irms[x];
                doc["ev_meter"]["currents"]["TOTAL"] = EVMeter.Irms[0] + EVMeter.Irms[1] + EVMeter.Irms[2];
            }

            if(request->hasParam("import_active_energy") && request->hasParam("export_active_energy") && request->hasParam("import_active_power")) {

                EVMeter.Import_active_energy = request->getParam("import_active_energy")->value().toInt();
                EVMeter.Export_active_energy = request->getParam("export_active_energy")->value().toInt();

                EVMeter.PowerMeasured = request->getParam("import_active_power")->value().toInt();
                EVMeter.UpdateEnergies();
                doc["ev_meter"]["import_active_power"] = EVMeter.PowerMeasured;
                doc["ev_meter"]["import_active_energy"] = EVMeter.Import_active_energy;
                doc["ev_meter"]["export_active_energy"] = EVMeter.Export_active_energy;
                doc["ev_meter"]["total_kwh"] = EVMeter.Energy;
                doc["ev_meter"]["charged_kwh"] = EVMeter.EnergyCharged;
            }
        }

        String json;
        serializeJson(doc, json);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\r\n", json.c_str());    // Yes. Respond JSON
        return true;
#if MODEM
    } else if (mg_http_match_uri(hm, "/ev_state") && !memcmp("POST", hm->method.buf, hm->method.len)) {
        DynamicJsonDocument doc(200);

        //State of charge posting
        int current_soc = request->getParam("current_soc")->value().toInt();
        int full_soc = request->getParam("full_soc")->value().toInt();

        // Energy requested by car
        int energy_request = request->getParam("energy_request")->value().toInt();

        // Total energy capacity of car's battery
        int energy_capacity = request->getParam("energy_capacity")->value().toInt();

        // Update EVCCID of car
        if (request->hasParam("evccid")) {
            if (request->getParam("evccid")->value().length() <= 32) {
                strncpy(EVCCID, request->getParam("evccid")->value().c_str(), sizeof(EVCCID));
                doc["evccid"] = EVCCID;
            }
        }

        if (full_soc >= FullSoC) // Only update if we received it, since sometimes it's there, sometimes it's not
            FullSoC = full_soc;

        if (energy_capacity >= EnergyCapacity) // Only update if we received it, since sometimes it's there, sometimes it's not
            EnergyCapacity = energy_capacity;

        if (energy_request >= EnergyRequest) // Only update if we received it, since sometimes it's there, sometimes it's not
            EnergyRequest = energy_request;

        if (current_soc >= 0 && current_soc <= 100) {
            // We set the InitialSoC for our own calculations
            InitialSoC = current_soc;

            // We also set the ComputedSoC to allow for app integrations
            ComputedSoC = current_soc;

            // Skip waiting, charge since we have what we've got
            if (State == STATE_MODEM_REQUEST || State == STATE_MODEM_WAIT || State == STATE_MODEM_DONE){
                _LOG_A("Received SoC via REST. Shortcut to State Modem Done\n");
                setState(STATE_MODEM_DONE); // Go to State B, which means in this case setting PWM
            }
        }

        RecomputeSoC();

        doc["current_soc"] = current_soc;
        doc["full_soc"] = full_soc;
        doc["energy_capacity"] = energy_capacity;
        doc["energy_request"] = energy_request;

        String json;
        serializeJson(doc, json);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\r\n", json.c_str());    // Yes. Respond JSON
        return true;
#endif

#if FAKE_RFID
    //this can be activated by: http://smartevse-xxx.lan/debug?showrfid=1
    } else if (mg_http_match_uri(hm, "/debug") && !memcmp("GET", hm->method.buf, hm->method.len)) {
        if(request->hasParam("showrfid")) {
            Show_RFID = strtol(request->getParam("showrfid")->value().c_str(),NULL,0);
        }
        _LOG_A("DEBUG: Show_RFID=%u.\n",Show_RFID);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\r\n", ""); //json request needs json response
        return true;
#endif

#if AUTOMATED_TESTING
    //this can be activated by: http://smartevse-xxx.lan/automated_testing?current_max=100
    //WARNING: because of automated testing, no limitations here!
    //THAT IS DANGEROUS WHEN USED IN PRODUCTION ENVIRONMENT
    //FOR SMARTEVSE's IN A TESTING BENCH ONLY!!!!
    } else if (mg_http_match_uri(hm, "/automated_testing") && !memcmp("POST", hm->method.buf, hm->method.len)) {
        if(request->hasParam("current_max")) {
            MaxCurrent = strtol(request->getParam("current_max")->value().c_str(),NULL,0);
        }
        if(request->hasParam("current_main")) {
            MaxMains = strtol(request->getParam("current_main")->value().c_str(),NULL,0);
        }
        if(request->hasParam("current_max_circuit")) {
            MaxCircuit = strtol(request->getParam("current_max_circuit")->value().c_str(),NULL,0);
        }
        if(request->hasParam("mainsmeter")) {
            MainsMeter.Type = strtol(request->getParam("mainsmeter")->value().c_str(),NULL,0);
        }
        if(request->hasParam("evmeter")) {
            EVMeter.Type = strtol(request->getParam("evmeter")->value().c_str(),NULL,0);
        }
        if(request->hasParam("config")) {
            Config = strtol(request->getParam("config")->value().c_str(),NULL,0);
            setState(STATE_A);                                                  // so the new value will actually be read
        }
        if(request->hasParam("loadbl")) {
            int LBL = strtol(request->getParam("loadbl")->value().c_str(),NULL,0);
#if SMARTEVSE_VERSION == 3
            ConfigureModbusMode(LBL);
#endif
            LoadBl = LBL;
        }
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\r\n", ""); //json request needs json response
        return true;
#endif
  }
  return false;
}


/*
 * OCPP-related function definitions
 */
#if ENABLE_OCPP

void ocppUpdateRfidReading(const unsigned char *uuid, size_t uuidLen) {
    if (!uuid || uuidLen > sizeof(OcppRfidUuid)) {
        _LOG_W("OCPP: invalid UUID\n");
        return;
    }
    memcpy(OcppRfidUuid, uuid, uuidLen);
    OcppRfidUuidLen = uuidLen;
    OcppLastRfidUpdate = millis();
}

bool ocppIsConnectorPlugged() {
    return OcppTrackCPvoltage >= PILOT_9V && OcppTrackCPvoltage <= PILOT_3V;
}

bool ocppHasTxNotification() {
    return OcppDefinedTxNotification && millis() - OcppLastTxNotification <= 3000;
}

MicroOcpp::TxNotification ocppGetTxNotification() {
    return OcppTrackTxNotification;
}

bool ocppLockingTxDefined() {
    return OcppLockingTx != nullptr;
}

void ocppInit() {

    //load OCPP library modules: Mongoose WS adapter and Core OCPP library

    auto filesystem = MicroOcpp::makeDefaultFilesystemAdapter(
            MicroOcpp::FilesystemOpt::Use_Mount_FormatOnFail // Enable FS access, mount LittleFS here, format data partition if necessary
            );

    OcppWsClient = new MicroOcpp::MOcppMongooseClient(
            &mgr,
            nullptr,    // OCPP backend URL (factory default)
            nullptr,    // ChargeBoxId (factory default)
            nullptr,    // WebSocket Basic Auth token (factory default)
            nullptr,    // CA cert (cert string must outlive WS client)
            filesystem);

    mocpp_initialize(
            *OcppWsClient, //WebSocket adapter for MicroOcpp
            ChargerCredentials("SmartEVSE", "Stegen Electronics", VERSION, String(serialnr).c_str(), NULL, (char *) EMConfig[EVMeter.Type].Desc),
            filesystem);

    //setup OCPP hardware bindings

    setEnergyMeterInput([] () { //Input of the electricity meter register in Wh
        return EVMeter.Energy;
    });

    setPowerMeterInput([] () { //Input of the power meter reading in W
        return EVMeter.PowerMeasured;
    });

    setConnectorPluggedInput([] () { //Input about if an EV is plugged to this EVSE
        return ocppIsConnectorPlugged();
    });

    setEvReadyInput([] () { //Input if EV is ready to charge (= J1772 State C)
        return OcppTrackCPvoltage >= PILOT_6V && OcppTrackCPvoltage <= PILOT_3V;
    });

    setEvseReadyInput([] () { //Input if EVSE allows charge (= PWM signal on)
        return GetCurrent() > 0; //PWM is enabled
    });

    addMeterValueInput([] () {
            return (float) (EVMeter.Irms[0] + EVMeter.Irms[1] + EVMeter.Irms[2])/10;
        },
        "Current.Import",
        "A");

    addMeterValueInput([] () {
            return (float) EVMeter.Irms[0]/10;
        },
        "Current.Import",
        "A",
        nullptr, // Location defaults to "Outlet"
        "L1");

    addMeterValueInput([] () {
            return (float) EVMeter.Irms[1]/10;
        },
        "Current.Import",
        "A",
        nullptr, // Location defaults to "Outlet"
        "L2");

    addMeterValueInput([] () {
            return (float) EVMeter.Irms[2]/10;
        },
        "Current.Import",
        "A",
        nullptr, // Location defaults to "Outlet"
        "L3");

    addMeterValueInput([] () {
            return (float)GetCurrent() * 0.1f;
        },
        "Current.Offered",
        "A");

    addMeterValueInput([] () {
            return (float)TempEVSE;
        },
        "Temperature",
        "Celsius");

#if MODEM
        addMeterValueInput([] () {
                return (float)ComputedSoC;
            },
            "SoC",
            "Percent");
#endif

    addErrorCodeInput([] () {
        return (ErrorFlags & TEMP_HIGH) ? "HighTemperature" : (const char*)nullptr;
    });

    addErrorCodeInput([] () {
        return (ErrorFlags & RCM_TRIPPED) ? "GroundFailure" : (const char*)nullptr;
    });

    addErrorDataInput([] () -> MicroOcpp::ErrorData {
        if (ErrorFlags & CT_NOCOMM) {
            MicroOcpp::ErrorData error = "PowerMeterFailure";
            error.info = "Communication with mains meter lost";
            return error;
        }
        return nullptr;
    });

    addErrorDataInput([] () -> MicroOcpp::ErrorData {
        if (ErrorFlags & EV_NOCOMM) {
            MicroOcpp::ErrorData error = "PowerMeterFailure";
            error.info = "Communication with EV meter lost";
            return error;
        }
        return nullptr;
    });

    // If SmartEVSE load balancer is turned off, then enable OCPP Smart Charging
    // This means after toggling LB, OCPP must be disabled and enabled for changes to become effective
    if (!LoadBl) {
        setSmartChargingCurrentOutput([] (float currentLimit) {
            OcppCurrentLimit = currentLimit; // Can be negative which means that no limit is defined

            // Re-evaluate charge rate and apply
            if (!LoadBl) { // Execute only if LB is still disabled

                CalcBalancedCurrent(0);
                if (IsCurrentAvailable()) {
                    // OCPP is the exclusive LB, clear LESS_6A error if set
                    ErrorFlags &= ~LESS_6A;
                    ChargeDelay = 0;
                }
                if ((State == STATE_B || State == STATE_C) && !CPDutyOverride) {
                    if (IsCurrentAvailable()) {
                        SetCurrent(ChargeCurrent);
                    } else {
                        setStatePowerUnavailable();
                    }
                }
            }
        });
    }

    setOnUnlockConnectorInOut([] () -> UnlockConnectorResult {
        // MO also stops transaction which should toggle OcppForcesLock false
        OcppLockingTx.reset();
        if (Lock == 0 || digitalRead(PIN_LOCK_IN) == lock2) {
            // Success
            return UnlockConnectorResult_Unlocked;
        }

        // No result yet, wait (MO eventually times out)
        return UnlockConnectorResult_Pending;
    });

    setOccupiedInput([] () -> bool {
        // Keep Finishing state while LockingTx effectively blocks new transactions
        return OcppLockingTx != nullptr;
    });

    setStopTxReadyInput([] () {
        // Stop value synchronization: block StopTransaction for 5 seconds to give the Modbus readings some time to come through
        return millis() - OcppStopReadingSyncTime >= 5000;
    });

    setTxNotificationOutput([] (MicroOcpp::Transaction*, MicroOcpp::TxNotification event) {
        OcppDefinedTxNotification = true;
        OcppTrackTxNotification = event;
        OcppLastTxNotification = millis();
    });

    OcppUnlockConnectorOnEVSideDisconnect = MicroOcpp::declareConfiguration<bool>("UnlockConnectorOnEVSideDisconnect", true);

    endTransaction(nullptr, "PowerLoss"); // If a transaction from previous power cycle is still running, abort it here
}

void ocppDeinit() {

    // Record stop value for transaction manually (normally MO would wait until `mocpp_loop()`, but that's too late here)
    if (auto& tx = getTransaction()) {
        if (tx->getMeterStop() < 0) {
            // Stop value not defined yet
            tx->setMeterStop(EVMeter.Import_active_energy); // Use same reading as in `setEnergyMeterInput()`
            tx->setStopTimestamp(getOcppContext()->getModel().getClock().now());
        }
    }

    endTransaction(nullptr, "Other"); // If a transaction is running, shut it down forcefully. The StopTx request will be sent when OCPP runs again.

    OcppUnlockConnectorOnEVSideDisconnect.reset();
    OcppLockingTx.reset();
    OcppForcesLock = false;

    if (OcppTrackPermitsCharge) {
        _LOG_A("OCPP unset Access_bit\n");
        setAccess(false);
    }

    OcppTrackPermitsCharge = false;
    OcppTrackAccessBit = false;
    OcppTrackCPvoltage = PILOT_NOK;
    OcppCurrentLimit = -1.f;

    mocpp_deinitialize();

    delete OcppWsClient;
    OcppWsClient = nullptr;
}

void ocppLoop() {

    if (pilot >= PILOT_12V && pilot <= PILOT_3V) {
        OcppTrackCPvoltage = pilot;
    }

    mocpp_loop();

    //handle RFID input

    if (OcppTrackLastRfidUpdate != OcppLastRfidUpdate) {
        // New RFID card swiped

        char uuidHex [2 * sizeof(OcppRfidUuid) + 1];
        uuidHex[0] = '\0';
        for (size_t i = 0; i < OcppRfidUuidLen; i++) {
            snprintf(uuidHex + 2*i, 3, "%02X", OcppRfidUuid[i]);
        }

        if (OcppLockingTx) {
            // Connector is still locked by earlier transaction

            if (!strcmp(uuidHex, OcppLockingTx->getIdTag())) {
                // Connector can be unlocked again
                OcppLockingTx.reset();
                endTransaction(uuidHex, "Local");
            } // else: Connector remains blocked for now
        } else if (getTransaction()) {
            //OCPP lib still has transaction (i.e. transaction running or authorization pending) --> swiping card again invalidates idTag
            endTransaction(uuidHex, "Local");
        } else {
            //OCPP lib has no idTag --> swiped card is used for new transaction
            OcppLockingTx = beginTransaction(uuidHex);
        }
    }
    OcppTrackLastRfidUpdate = OcppLastRfidUpdate;

    // Set / unset Access_bit
    // Allow to set Access_bit only once per OCPP transaction because other modules may override the Access_bit
    // Doesn't apply if SmartEVSE built-in RFID store is enabled
    if (RFIDReader == 6 || RFIDReader == 0) {
        // RFID reader in OCPP mode or RFID fully disabled - OCPP controls Access_bit
        if (!OcppTrackPermitsCharge && ocppPermitsCharge()) {
            _LOG_A("OCPP set Access_bit\n");
            setAccess(true);
        } else if (Access_bit && !ocppPermitsCharge()) {
            _LOG_A("OCPP unset Access_bit\n");
            setAccess(false);
        }
        OcppTrackPermitsCharge = ocppPermitsCharge();

        // Check if OCPP charge permission has been revoked by other module
        if (OcppTrackPermitsCharge && // OCPP has set Acess_bit and still allows charge
                !Access_bit) { // Access_bit is not active anymore
            endTransaction(nullptr, "Other");
        }
    } else {
        // Built-in RFID store enabled - OCPP does not control Access_bit, but starts transactions when Access_bit is set
        if (Access_bit && !OcppTrackAccessBit && !getTransaction() && isOperative()) {
            // Access_bit has been set
            OcppTrackAccessBit = true;
            _LOG_A("OCPP detected Access_bit set\n");
            char buf[15];
            if (RFID[0] == 0x01) {  // old reader 6 byte UID starts at RFID[1]
                sprintf(buf, "%02X%02X%02X%02X%02X%02X", RFID[1], RFID[2], RFID[3], RFID[4], RFID[5], RFID[6]);
            } else {
                sprintf(buf, "%02X%02X%02X%02X%02X%02X%02X", RFID[0], RFID[1], RFID[2], RFID[3], RFID[4], RFID[5], RFID[6]);
            }
            beginTransaction_authorized(buf);
        } else if (!Access_bit && (OcppTrackAccessBit || (getTransaction() && getTransaction()->isActive()))) {
            OcppTrackAccessBit = false;
            _LOG_A("OCPP detected Access_bit unset\n");
            char buf[15];
            if (RFID[0] == 0x01) {  // old reader 6 byte UID starts at RFID[1]
                sprintf(buf, "%02X%02X%02X%02X%02X%02X", RFID[1], RFID[2], RFID[3], RFID[4], RFID[5], RFID[6]);
            } else {
                sprintf(buf, "%02X%02X%02X%02X%02X%02X%02X", RFID[0], RFID[1], RFID[2], RFID[3], RFID[4], RFID[5], RFID[6]);
            }
            endTransaction_authorized(buf);
        }
    }

    // Stop value synchronization: block StopTransaction for a short period as long as charging is permitted
    if (ocppPermitsCharge()) {
        OcppStopReadingSyncTime = millis();
    }

    auto& transaction = getTransaction(); // Common tx which OCPP is currently processing (or nullptr if no tx is ongoing)

    // Check if Locking Tx has been invalidated by something other than RFID swipe
    if (OcppLockingTx) {
        if (OcppUnlockConnectorOnEVSideDisconnect->getBool() && !OcppLockingTx->isActive()) {
            // No LockingTx mode configured (still, keep LockingTx until end of transaction because the config could be changed in the middle of tx)
            OcppLockingTx.reset();
        } else if (OcppLockingTx->isAborted()) {
            // LockingTx hasn't successfully started
            OcppLockingTx.reset();
        } else if (transaction && transaction != OcppLockingTx) {
            // Another Tx has already started
            OcppLockingTx.reset();
        } else if (digitalRead(PIN_LOCK_IN) == lock2 && !OcppLockingTx->isActive()) {
            // Connector is has been unlocked and LockingTx has already run
            OcppLockingTx.reset();
        } // There may be further edge cases
    }

    OcppForcesLock = false;

    if (transaction && transaction->isAuthorized() && (transaction->isActive() || transaction->isRunning()) && // Common tx ongoing
            (OcppTrackCPvoltage >= PILOT_9V && OcppTrackCPvoltage <= PILOT_3V)) { // Connector plugged
        OcppForcesLock = true;
    }

    if (OcppLockingTx && OcppLockingTx->getStartSync().isRequested()) { // LockingTx goes beyond tx completion
        OcppForcesLock = true;
    }

}
#endif //ENABLE_OCPP


void setup() {
#if SMARTEVSE_VERSION == 3

    pinMode(PIN_CP_OUT, OUTPUT);            // CP output
    //pinMode(PIN_SW_IN, INPUT);            // SW Switch input, handled by OneWire32 class
    pinMode(PIN_SSR, OUTPUT);               // SSR1 output
    pinMode(PIN_SSR2, OUTPUT);              // SSR2 output
    pinMode(PIN_RCM_FAULT, INPUT_PULLUP);   

    pinMode(PIN_LCD_LED, OUTPUT);           // LCD backlight
    pinMode(PIN_LCD_RST, OUTPUT);           // LCD reset
    pinMode(PIN_IO0_B1, INPUT);             // < button
    pinMode(PIN_LCD_A0_B2, OUTPUT);         // o Select button + A0 LCD
    pinMode(PIN_LCD_SDO_B3, OUTPUT);        // > button + SDA/MOSI pin

    pinMode(PIN_LOCK_IN, INPUT);            // Locking Solenoid input
    pinMode(PIN_LEDR, OUTPUT);              // Red LED output
    pinMode(PIN_LEDG, OUTPUT);              // Green LED output
    pinMode(PIN_LEDB, OUTPUT);              // Blue LED output
    pinMode(PIN_ACTA, OUTPUT);              // Actuator Driver output R
    pinMode(PIN_ACTB, OUTPUT);              // Actuator Driver output W
    pinMode(PIN_CPOFF, OUTPUT);             // Disable CP output (active high)
    pinMode(PIN_RS485_RX, INPUT);
    pinMode(PIN_RS485_TX, OUTPUT);
    pinMode(PIN_RS485_DIR, OUTPUT);

    digitalWrite(PIN_LEDR, LOW);
    digitalWrite(PIN_LEDG, LOW);
    digitalWrite(PIN_LEDB, LOW);
    digitalWrite(PIN_ACTA, LOW);
    digitalWrite(PIN_ACTB, LOW);        
    digitalWrite(PIN_SSR, LOW);             // SSR1 OFF
    digitalWrite(PIN_SSR2, LOW);            // SSR2 OFF
    digitalWrite(PIN_LCD_LED, HIGH);        // LCD Backlight ON
    CP_OFF;           // CP signal OFF

 
    // Uart 0 debug/program port
    Serial.begin(115200);
    while (!Serial);
    _LOG_A("SmartEVSE v3 powerup\n");

    // configure SPI connection to LCD
    // only the SPI_SCK and SPI_MOSI pins are used
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SPI_SS);
    // the ST7567's max SPI Clock frequency is 20Mhz at 3.3V/25C
    // We choose 10Mhz here, to reserve some room for error.
    // SPI mode is MODE3 (Idle = HIGH, clock in on rising edge)
    SPI.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE3));
    

    // The CP (control pilot) output is a fixed 1khz square-wave (+6..9v / -12v).
    // It's pulse width varies between 10% and 96% indicating 6A-80A charging current.
    // to detect state changes we should measure the CP signal while it's at ~5% (so 50uS after the positive pulse started)
    // we use an i/o interrupt at the CP pin output, and a one shot timer interrupt to start the ADC conversion.
    // would be nice if there was an easier way...

    // setup timer, and one shot timer interrupt to 50us
    timerA = timerBegin(0, 80, true);
    timerAttachInterrupt(timerA, &onTimerA, false);
    // we start in STATE A, with a static +12V CP signal
    // set alarm to trigger every 1mS, and let it reload every 1ms
    timerAlarmWrite(timerA, PWM_100, true);
    // when PWM is active, we sample the CP pin after 5% 
    timerAlarmEnable(timerA);


    // Setup ADC on CP, PP and Temperature pin
    adc1_config_width(ADC_WIDTH_BIT_10);                                    // 10 bits ADC resolution is enough
    adc1_config_channel_atten(ADC1_CHANNEL_3, ADC_ATTEN_DB_12);             // setup the CP pin input attenuation to 11db
    adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_6);              // setup the PP pin input attenuation to 6db
    adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_6);              // setup the Temperature input attenuation to 6db

    //Characterize the ADC at particular attentuation for each channel
    adc_chars_CP = (esp_adc_cal_characteristics_t *) calloc(1, sizeof(esp_adc_cal_characteristics_t));
    adc_chars_PP = (esp_adc_cal_characteristics_t *) calloc(1, sizeof(esp_adc_cal_characteristics_t));
    adc_chars_Temperature = (esp_adc_cal_characteristics_t *) calloc(1, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_value_t val_type = esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_10, 1100, adc_chars_CP);
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_6, ADC_WIDTH_BIT_10, 1100, adc_chars_PP);
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_6, ADC_WIDTH_BIT_10, 1100, adc_chars_Temperature);
          
    
    // Setup PWM on channel 0, 1000Hz, 10 bits resolution
    ledcSetup(CP_CHANNEL, 1000, 10);            // channel 0  => Group: 0, Channel: 0, Timer: 0
    // setup the RGB led PWM channels
    // as PWM channel 1 is used by the same timer as the CP timer (channel 0), we start with channel 2
    ledcSetup(RED_CHANNEL, 5000, 8);            // R channel 2, 5kHz, 8 bit
    ledcSetup(GREEN_CHANNEL, 5000, 8);          // G channel 3, 5kHz, 8 bit
    ledcSetup(BLUE_CHANNEL, 5000, 8);           // B channel 4, 5kHz, 8 bit
    ledcSetup(LCD_CHANNEL, 5000, 8);            // LCD channel 5, 5kHz, 8 bit

    // attach the channels to the GPIO to be controlled
    ledcAttachPin(PIN_CP_OUT, CP_CHANNEL);      
    //pinMode(PIN_CP_OUT, OUTPUT);                // Re-init the pin to output, required in order for attachInterrupt to work (2.0.2)
                                                // not required/working on master branch..
                                                // see https://github.com/espressif/arduino-esp32/issues/6140
    ledcAttachPin(PIN_LEDR, RED_CHANNEL);
    ledcAttachPin(PIN_LEDG, GREEN_CHANNEL);
    ledcAttachPin(PIN_LEDB, BLUE_CHANNEL);
    ledcAttachPin(PIN_LCD_LED, LCD_CHANNEL);

    SetCPDuty(1024);                            // channel 0, duty cycle 100%
    ledcWrite(RED_CHANNEL, 255);
    ledcWrite(GREEN_CHANNEL, 0);
    ledcWrite(BLUE_CHANNEL, 255);
    ledcWrite(LCD_CHANNEL, 0);

    // Setup PIN interrupt on rising edge
    // the timer interrupt will be reset in the ISR.
    attachInterrupt(PIN_CP_OUT, onCPpulse, RISING);   
   
    // Uart 1 is used for Modbus @ 9600 8N1
    RTUutils::prepareHardwareSerial(Serial1);
    Serial1.begin(MODBUS_BAUDRATE, SERIAL_8N1, PIN_RS485_RX, PIN_RS485_TX);

   
    //Check type of calibration value used to characterize ADC
    _LOG_A("Checking eFuse Vref settings: ");
    if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
        _LOG_A_NO_FUNC("OK\n");
    } else if (val_type == ESP_ADC_CAL_VAL_EFUSE_TP) {
        _LOG_A_NO_FUNC("Two Point\n");
    } else {
        _LOG_A_NO_FUNC("not programmed!!!\n");
    }
    

#else //SMARTEVSE_VERSION
    uint8_t writeValue;
    uint8_t readValue;
    uint16_t reg16;

    //lower the CPU frequency to 160, 80, 40 MHz
    setCpuFrequencyMhz(160);

    pinMode(PIN_QCA700X_CS, OUTPUT);           // SPI_CS QCA7005
    pinMode(PIN_QCA700X_INT, INPUT);           // SPI_INT QCA7005
    pinMode(SPI_SCK, OUTPUT);
    pinMode(SPI_MISO, INPUT);
    pinMode(SPI_MOSI, OUTPUT);
    pinMode(PIN_QCA700X_RESETN, OUTPUT);

    pinMode(RTC_SCL, INPUT);                  // set to input for now...
    pinMode(RTC_SDA, INPUT);
    pinMode(RTC_INT, INPUT);

    pinMode(BUTTON1, INPUT_PULLUP);
    pinMode(BUTTON3, INPUT_PULLUP);

    pinMode(LCD_LED, OUTPUT);               // LCD backlight
    pinMode(LCD_RST, OUTPUT);               // LCD reset, active high
    pinMode(LCD_SDA, OUTPUT);               // LCD Data
    pinMode(LCD_SCK, OUTPUT);               // LCD Clock
    pinMode(LCD_A0_B2, OUTPUT);             // Select button + A0 LCD
    pinMode(LCD_CS, OUTPUT);

    pinMode(WCH_SWDIO, INPUT);              // WCH-Link (unused/unconnected)
    pinMode(WCH_SWCLK, INPUT);              // WCH-Link (unused) / BOOT0 select
    pinMode(WCH_NRST, INPUT);               // WCH NRST


    // shutdown QCA is done by the WCH32V, we set all IO pins low, so no current is flowing into the powered down chip.
    digitalWrite(PIN_QCA700X_CS, LOW);
    digitalWrite(PIN_QCA700X_RESETN, LOW);
    digitalWrite(SPI_SCK, LOW);
    digitalWrite(SPI_MOSI, LOW);


    //digitalWrite(PIN_QCA700X_RESETN, HIGH);     // Active Low
    //digitalWrite(PIN_QCA700X_CS, HIGH);

    // configure SPI connection to QCA modem
    QCA_SPI1.begin(SPI_SCK, SPI_MISO, SPI_MOSI, PIN_QCA700X_CS);
    // SPI mode is MODE3 (Idle = HIGH, clock in on rising edge), we use a 10Mhz SPI clock
    QCA_SPI1.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE3));
    //attachInterrupt(digitalPinToInterrupt(PIN_QCA700X_INT), SPI_InterruptHandler, RISING);


    // Setup SWDIO pin as Power Panic interrupt received from the WCH uC. (unused, we use serial comm)
    //attachInterrupt(WCH_SWDIO, PowerPanicESP, FALLING);


    Serial.begin();                                                     // Debug output on USB
    Serial.setTxTimeoutMs(1);                                           // Workaround for Serial.print while unplugged USB.
                                                                        // log_d does not have this issue?
    Serial1.begin(115200, SERIAL_8N1, USART_RX, USART_TX, false);       // Serial connection to main board microcontroller
    //Serial2.begin(115200, SERIAL_8N1, USART_TX, -1, false);
    Serial.printf("\nSmartEVSE v4 powerup\n");

    _LOG_D("Total heap: %u.\n", ESP.getHeapSize());
    _LOG_D("Free heap: %u.\n", ESP.getFreeHeap());
    _LOG_D("Flash Size: %u.\n", ESP.getFlashChipSize());
    _LOG_D("Total PSRAM: %u.\n", ESP.getPsramSize());
    _LOG_D("Free PSRAM: %u.\n", ESP.getFreePsram());



    // configure SPI connection to LCD
    // SPI_SCK, SPI_MOSI and LCD_CS pins are used.
    LCD_SPI2.begin(LCD_SCK, -1, LCD_SDA, LCD_CS);
    // the ST7567's max SPI Clock frequency is 20Mhz at 3.3V/25C
    // We choose 10Mhz here, to reserve some room for error.
    // SPI mode is MODE3 (Idle = HIGH, clock in on rising edge)
    LCD_SPI2.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE3));
    // Dummy transaction, to make sure SCLK idles high (IDF bug?)
    LCD_SPI2.transfer(0);
    _LOG_D("SPI for LCD configured.\n");

    //GLCD_init();                                // Initialize LCD


    ledcSetup(LCD_CHANNEL, 5000, 8);            // LCD channel 5, 5kHz, 8 bit
    ledcAttachPin(LCD_LED, LCD_CHANNEL);
    ledcWrite(LCD_CHANNEL, 255);                // Set LCD backlight brightness 0-255


    //delay(3000);

    // Initialize and create the RTC device
    Wire.setPins(RTC_SDA, RTC_SCL);
    Wire.begin();
    rtc.initI2C();

    // Read powerdown timestamp values from RTC
    rtcTS = {
        rtc.readFromRegister(STATUS_REGISTER_ADDRESS),
        rtc.getTSHour(),
        rtc.getTSMinute(),
        rtc.getTSSecond(),
        rtc.getTSDate(),
        rtc.getTSMonth(),
        rtc.getTSYear() };

    // First power on, or backup power < 0.8V
    if (rtcTS.Status & 0x01) _LOG_D("rtc power on Reset, data unitialized!\n");
    // Event Flag set?
    if (rtcTS.Status & 0x02) {
        _LOG_D("Powerdown Timestamp %2u:%02u:%02u %u-%u-%u \n",rtcTS.Hour, rtcTS.Minute, rtcTS.Second, rtcTS.Date, rtcTS.Month, rtcTS.Year);
    }

    // Setup the device to use the EEPROM memory
    rtc.useEEPROM(true);                                      // disable 24H EEprom refresh before modifying EEprom

    rtc.writeToRegister(STATUS_REGISTER_ADDRESS, 0);          // reset Status register
    rtc.writeToRegister(CONTROL2_REGISTER_ADDRESS, 0);        // TSE bit 0, EIE = 0, 24H clock
    rtc.writeToRegister(0x13, 5);                             // Set TSR bit, to reset all Time Stamp registers to 00h.
                                                              // TSS bit 1, Automatic Backup Switchover is selected as Time Stamp source.
    rtc.writeToRegister(CONTROL2_REGISTER_ADDRESS, 0x80);     // TSE bit 1, enable the Time Stamp function

    readValue = rtc.readFromRegister(0x37);                   // preserve MSB as it's part of the crystal calibration
    writeValue = (readValue & 0x80) | RTCBackupSource;        // Level Switching Mode (LSM) is used when operating from Battery as Vbat > Vdd (Idd=115nA)
                                                              // Direct Switching Mode (DSM) is used with the supercap
                                                              // Trickle charge is only enabled when Supercap is used as backup source. (Idd=95nA)
    if (writeValue != readValue) {

        rtc.writeToRegister(0x37, writeValue);                // Write settings
        //readValue = rtc.readFromRegister(0x37) & 0x7f;        // ignore msb

        rtc.waitforEEPROM();                                  // wait for EEprom to be ready.

        rtc.writeToRegister(EEPROM_COMMAND_ADDRESS, 0x00);    // Refresh Ram -> EEprom
        rtc.writeToRegister(EEPROM_COMMAND_ADDRESS, 0x11);    // Refresh Ram -> EEprom
    }
    readValue = readValue & 0x7f;

    if (readValue == SUPERCAP) _LOG_D("RTC: Charging capacitor.\n");
    else if (readValue == BATTERY) _LOG_D("RTC: Battery configured.\n");
    else _LOG_A("RTC: capacitor not Charging!\n");

    rtc.andOrRegister(CONTROL1_REGISTER_ADDRESS, 0xf7, 0x00); // re-activate 24H EEprom refresh

    // RTC time valid?
    if ((rtcTS.Status & 0x01) == 0) {
        _LOG_D("Powerup at %2u:%02u:%02u %u-%u-%u \n",rtc.getHour(), rtc.getMinute(), rtc.getSecond(), rtc.getDate(), rtc.getMonth(), rtc.getYear());

        // read rtc, and update timeinfo struct
        timeinfo.tm_year = rtc.getYear() -1900;
        timeinfo.tm_mon = rtc.getMonth() -1;
        timeinfo.tm_wday = rtc.getWeekday();
        timeinfo.tm_mday = rtc.getDate();
        timeinfo.tm_hour = rtc.getHour();
        timeinfo.tm_min = rtc.getMinute();
        timeinfo.tm_sec = rtc.getSecond();

    }

/*
    if (WchFirmwareUpdate()) {
        _LOG_A("Firmware update failed.\n");
    } else _LOG_D("WCH programming done\n");
*/
    // should not be needed to reset the WCH ic at powerup/reset on the production version.
    _LOG_D("reset WCH ic\n");
    WchReset();


    // After powerup request WCH version (version?)
    // then send Configuration to WCH

    Config = 0;         // Configuration (0:Socket / 1:Fixed Cable)
    Mode = 1;           // EVSE mode (0:Normal / 1:Smart / 2:Solar)
    Lock = 1;           // Cable lock (0:Disable / 1:Solenoid / 2:Motor)
    Switch = 3;         // External Switch (0:Disable / 1:Access B / 2:Access S / 3:Smart-Solar B / 4:Smart-Solar S)
    RFIDReader = 1;     // RFID Reader (0:Disabled / 1:Enabled / 2:Enable One / 3:Learn / 4:Delete / 5:Delete All)
    RCmon = 0;          // Residual Current Monitor (0:Disable / 1:Enable)
    PwrPanic = 0;       // Enable PowerPanic feature
    LoadBl = 3;         // Set to Node 2
    ModemPwr = 1;       // Modem Power ON
    Initialized = 1;    // Set Initialized to 1


#endif //SMARTEVSE_VERSION
    // Read all settings from non volatile memory; MQTTprefix will be overwritten if stored in NVS
    read_settings();                                                            // initialize with default data when starting for the first time
    validate_settings();
    ReadRFIDlist();                                                             // Read all stored RFID's from storage

    getButtonState();
/*     * @param Buttons: < o >
 *          Value: 1 2 4
 *            Bit: 0:Pressed / 1:Released         */
    // Sample middle+right button, and lock/unlock LCD buttons.
    if (ButtonState == 1) {
        LCDlock = !LCDlock;
        write_settings();
    }

    // Create Task EVSEStates, that handles changes in the CP signal
    xTaskCreate(
        Timer10ms,      // Function that should be called
        "Timer10ms",    // Name of the task (for debugging)
        4096,           // Stack size (bytes)                              // printf needs atleast 1kb
        NULL,           // Parameter to pass
        5,              // Task priority - high
        NULL            // Task handle
    );

#if SMARTEVSE_VERSION == 3
    // Create Task BlinkLed (10ms)
    xTaskCreate(
        BlinkLed,       // Function that should be called
        "BlinkLed",     // Name of the task (for debugging)
        2048,           // Stack size (bytes)                              // printf needs atleast 1kb
        NULL,           // Parameter to pass
        1,              // Task priority - low
        NULL            // Task handle
    );

    // Create Task 100ms Timer
    xTaskCreate(
        Timer100ms,     // Function that should be called
        "Timer100ms",   // Name of the task (for debugging)
        4608,           // Stack size (bytes)
        NULL,           // Parameter to pass
        3,              // Task priority - medium
        NULL            // Task handle
    );
#else //SMARTEVSE_VERSION
    // Search for QCA modem
    //
    digitalWrite(PIN_QCA700X_RESETN, HIGH);         // get modem out of reset
    _LOG_D("Searching for modem.. \n");

    do {
        reg16 = qcaspi_read_register16(SPI_REG_SIGNATURE);
        if (reg16 == QCASPI_GOOD_SIGNATURE) {
            _LOG_D("QCA700X modem found\n");
        } else delay(500);
    } while (reg16 != QCASPI_GOOD_SIGNATURE);

#endif //SMARTEVSE_VERSION

    // Create Task Second Timer (1000ms)
    xTaskCreate(
        Timer1S,        // Function that should be called
        "Timer1S",      // Name of the task (for debugging)
        4096,           // Stack size (bytes)                              
        NULL,           // Parameter to pass
        3,              // Task priority - medium
        NULL            // Task handle
    );

    WiFiSetup();

#if SMARTEVSE_VERSION == 3
    // Set eModbus LogLevel to 1, to suppress possible E5 errors
    MBUlogLvl = LOG_LEVEL_CRITICAL;
    ConfigureModbusMode(255);
#endif

    BacklightTimer = BACKLIGHT;
    GLCD_init();

#if SMARTEVSE_VERSION == 3
    CP_ON;           // CP signal ACTIVE
#endif

    firmwareUpdateTimer = random(FW_UPDATE_DELAY, 0xffff);
}


// returns true if current and latest version can be detected correctly and if the latest version is newer then current
// this means that ANY home compiled version, which has version format "11:20:03@Jun 17 2024", will NEVER be automatically updated!!
// same goes for current version with an -RC extension: this will NEVER be automatically updated!
// same goes for latest version with an -RC extension: this will NEVER be automatically updated! This situation should never occur since
// we only update from the "stable" repo !!
bool fwNeedsUpdate(char * version) {
    // version NEEDS to be in the format: vx.y.z[-RCa] where x, y, z, a are digits, multiple digits are allowed.
    // valid versions are v3.6.10   v3.17.0-RC13
    int latest_major, latest_minor, latest_patch, latest_rc, cur_major, cur_minor, cur_patch, cur_rc;
    int hit = sscanf(version, "v%i.%i.%i-RC%i", &latest_major, &latest_minor, &latest_patch, &latest_rc);
    _LOG_A("Firmware version detection hit=%i, LATEST version detected=v%i.%i.%i-RC%i.\n", hit, latest_major, latest_minor, latest_patch, latest_rc);
    int hit2 = sscanf(VERSION, "v%i.%i.%i-RC%i", &cur_major, &cur_minor, &cur_patch, &cur_rc);
    _LOG_A("Firmware version detection hit=%i, CURRENT version detected=v%i.%i.%i-RC%i.\n", hit2, cur_major, cur_minor, cur_patch, cur_rc);
    if (hit != 3 || hit2 != 3)                                                  // we couldnt detect simple vx.y.z version nrs, either current or latest
        return false;
    if (cur_major > latest_major)
        return false;
    if (cur_major < latest_major)
        return true;
    if (cur_major == latest_major) {
        if (cur_minor > latest_minor)
            return false;
        if (cur_minor < latest_minor)
            return true;
        if (cur_minor == latest_minor)
            return (cur_patch < latest_patch);
    }
    return false;
}


void loop() {

    network_loop();
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck >= 1000) {
        lastCheck = millis();
        //this block is for non-time critical stuff that needs to run approx 1 / second

        // a reboot is requested, but we kindly wait until no EV connected
#if SMARTEVSE_VERSION == 3 //TODO
        if (shouldReboot && State == STATE_A) {                                 //slaves in STATE_C continue charging when Master reboots
#else  //SMARTEVSE_VERSION
        if (shouldReboot) {
#endif //SMARTEVSE_VERSION
            delay(5000);                                                        //give user some time to read any message on the webserver
            ESP.restart();
        }

#if SMARTEVSE_VERSION == 3 //TODO
        // TODO move this to a once a minute loop?
        if (DelayedStartTime.epoch2 && LocalTimeSet) {
            // Compare the times
            time_t now = time(nullptr);             //get current local time
            DelayedStartTime.diff = DelayedStartTime.epoch2 - (mktime(localtime(&now)) - EPOCH2_OFFSET);
            if (DelayedStartTime.diff > 0) {
                if (Access_bit != 0 && (DelayedStopTime.epoch2 == 0 || DelayedStopTime.epoch2 > DelayedStartTime.epoch2))
                    setAccess(0);                         //switch to OFF, we are Delayed Charging
            }
            else {
                //starttime is in the past so we are NOT Delayed Charging, or we are Delayed Charging but the starttime has passed!
                if (DelayedRepeat == 1)
                    DelayedStartTime.epoch2 += 24 * 3600;                           //add 24 hours so we now have a new starttime
                else
                    DelayedStartTime.epoch2 = DELAYEDSTARTTIME;
                setAccess(1);
            }
        }
        //only update StopTime.diff if starttime has already passed
        if (DelayedStopTime.epoch2 && LocalTimeSet) {
            // Compare the times
            time_t now = time(nullptr);             //get current local time
            DelayedStopTime.diff = DelayedStopTime.epoch2 - (mktime(localtime(&now)) - EPOCH2_OFFSET);
            if (DelayedStopTime.diff <= 0) {
                //DelayedStopTime has passed
                if (DelayedRepeat == 1)                                         //we are on a daily repetition schedule
                    DelayedStopTime.epoch2 += 24 * 3600;                        //add 24 hours so we now have a new starttime
                else
                    DelayedStopTime.epoch2 = DELAYEDSTOPTIME;
                setAccess(0);                         //switch to OFF
            }
        }
        //_LOG_A("DINGO: firmwareUpdateTimer just before decrement=%i.\n", firmwareUpdateTimer);
        if (AutoUpdate && !shouldReboot) {                                      // we don't want to autoupdate if we are on the verge of rebooting
            firmwareUpdateTimer--;
            char version[32];
            if (firmwareUpdateTimer == FW_UPDATE_DELAY) {                       // we now have to check for a new version
                //timer is not reset, proceeds to 65535 which is approx 18h from now
                if (getLatestVersion(String(String(OWNER_FACT) + "/" + String(REPO_FACT)), "", version)) {
                    if (fwNeedsUpdate(version)) {
                        _LOG_A("Firmware reports it needs updating, will update in %i seconds\n", FW_UPDATE_DELAY);
                        asprintf(&downloadUrl, "%s/fact_firmware.signed.bin", FW_DOWNLOAD_PATH); //will be freed in FirmwareUpdate() ; format: http://s3.com/fact_firmware.debug.signed.bin
                    } else {
                        _LOG_A("Firmware reports it needs NO update!\n");
                        firmwareUpdateTimer = random(FW_UPDATE_DELAY + 36000, 0xffff);  // at least 10 hours in between checks
                    }
                }
            } else if (firmwareUpdateTimer == 0) {                              // time to download & flash!
                if (getLatestVersion(String(String(OWNER_FACT) + "/" + String(REPO_FACT)), "", version)) { // recheck version info
                    if (fwNeedsUpdate(version)) {
                        _LOG_A("Firmware reports it needs updating, starting update NOW!\n");
                        asprintf(&downloadUrl, "%s/fact_firmware.signed.bin", FW_DOWNLOAD_PATH); //will be freed in FirmwareUpdate() ; format: http://s3.com/fact_firmware.debug.signed.bin
                        RunFirmwareUpdate();
                    } else {
                        _LOG_A("Firmware changed its mind, NOW it reports it needs NO update!\n");
                    }
                    //note: the firmwareUpdateTimer will decrement to 65535s so next check will be in 18hours or so....
                }
            }
        } // AutoUpdate
#endif //SMARTEVSE_VERSION
        /////end of non-time critical stuff
    }

    //OCPP lifecycle management
#if ENABLE_OCPP
    if (OcppMode && !getOcppContext()) {
        ocppInit();
    } else if (!OcppMode && getOcppContext()) {
        ocppDeinit();
    }

    if (OcppMode) {
        ocppLoop();
    }
#endif //ENABLE_OCPP

}
