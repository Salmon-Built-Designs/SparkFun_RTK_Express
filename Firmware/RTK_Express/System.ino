//Starting and restarting BT is a problem. See issue: https://github.com/espressif/arduino-esp32/issues/2718
//To work around the bug without modifying the core we create our own btStop() function with
//the patch from github
//bool customBTstop() {
//  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
//    return true;
//  }
//  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) {
//    if (esp_bt_controller_disable()) {
//      log_e("BT Disable failed");
//      return false;
//    }
//    while (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED);
//  }
//  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED)
//  {
//    log_i("inited");
//    if (esp_bt_controller_deinit())
//    {
//      log_e("BT deint failed");
//      return false;
//    }
//    while (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED)
//      ;
//    return true;
//  }
//  log_e("BT Stop failed");
//  return false;
//}

//If the phone has any new data (NTRIP RTCM, etc), read it in over Bluetooth and pass along to ZED
//Task for writing to the GNSS receiver
void F9PSerialWriteTask(void *e)
{
  while (true)
  {
    //Receive corrections from either the ESP32 USB or bluetooth
    //and write to the GPS
    //    if (Serial.available())
    //    {
    //      auto s = Serial.readBytes(wBuffer, SERIAL_SIZE_RX);
    //      GPS.write(wBuffer, s);
    //    }

    if (SerialBT.available())
    {
      while (SerialBT.available())
      {
        if (inTestMode == false)
        {
          //Pass bytes tp GNSS receiver
          auto s = SerialBT.readBytes(wBuffer, SERIAL_SIZE_RX);
          GPS.write(wBuffer, s);
        }
        else
        {
          Serial.printf("I heard: %c\n", SerialBT.read());
        }
      }
    }

    taskYIELD();
  }
}

//If the ZED has any new NMEA data, pass it out over Bluetooth
//Task for reading data from the GNSS receiver.
void F9PSerialReadTask(void *e)
{
  while (true)
  {
    if (GPS.available())
    {
      auto s = GPS.readBytes(rBuffer, SERIAL_SIZE_RX);

      //If we are survey-in then do not pass NMEA data from ZED to phone
      if (systemState == STATE_BASE_TEMP_SURVEY_NOT_STARTED || systemState == STATE_BASE_TEMP_SURVEY_STARTED)
      {
        //Do nothing
      }
      else if (SerialBT.connected())
      {
        SerialBT.write(rBuffer, s);
      }

      //If user wants to log, record to SD
      if (settings.zedOutputLogging == true)
      {
        if (online.microSD == true)
        {
          //Check if we are inside the max time window for logging
          if ((systemTime_minutes - startLogTime_minutes) < settings.maxLogTime_minutes)
          {
            taskYIELD();
            gnssDataFile.write(rBuffer, s);
            taskYIELD();

            //Force sync every 500ms
            if (millis() - lastDataLogSyncTime > 500)
            {
              lastDataLogSyncTime = millis();

              taskYIELD();
              gnssDataFile.sync();
              taskYIELD();

              if (settings.frequentFileAccessTimestamps == true)
                updateDataFileAccess(&gnssDataFile); // Update the file access time & date
            }
          }
        }
      }
    }
    taskYIELD();
  }
}

//Setup the u-blox module for any setup (base or rover)
//In general we check if the setting is incorrect before writing it. Otherwise, the set commands have, on rare occasion, become
//corrupt. The worst is when the I2C port gets turned off or the I2C address gets borked. We should only have to configure
//a fresh u-blox module once and never again.
bool configureUbloxModule()
{
  boolean response = true;

#define OUTPUT_SETTING 14
#define INPUT_SETTING 12

  //UART1 will primarily be used to pass NMEA from ZED to ESP32 (eventually to cell phone)
  //but the phone can also provide RTCM data. So let's be sure to enable RTCM on UART1 input.
  //In addition UART1 may output RAWX (for logging then PPP) so enable UBX on output
  getPortSettings(COM_PORT_UART1); //Load the settingPayload with this port's settings
  if (settingPayload[OUTPUT_SETTING] != (COM_TYPE_NMEA | COM_TYPE_UBX) || settingPayload[INPUT_SETTING] != COM_TYPE_RTCM3)
  {
    response &= myGNSS.setPortOutput(COM_PORT_UART1, COM_TYPE_NMEA | COM_TYPE_UBX); //Set the UART1 to output NMEA and UBX
    response &= myGNSS.setPortInput(COM_PORT_UART1, COM_TYPE_RTCM3); //Set the UART1 to input RTCM
  }

  //Disable SPI port - This is just to remove some overhead by ZED
  getPortSettings(COM_PORT_SPI); //Load the settingPayload with this port's settings
  if (settingPayload[OUTPUT_SETTING] != 0 || settingPayload[INPUT_SETTING] != 0)
  {
    response &= myGNSS.setPortOutput(COM_PORT_SPI, 0); //Disable all protocols
    response &= myGNSS.setPortInput(COM_PORT_SPI, 0); //Disable all protocols
  }

  getPortSettings(COM_PORT_UART2); //Load the settingPayload with this port's settings
  if (settingPayload[OUTPUT_SETTING] != COM_TYPE_RTCM3 || settingPayload[INPUT_SETTING] != COM_TYPE_RTCM3)
  {
    response &= myGNSS.setPortOutput(COM_PORT_UART2, COM_TYPE_RTCM3); //Set the UART2 to output RTCM (in case this device goes into base mode)
    response &= myGNSS.setPortInput(COM_PORT_UART2, COM_TYPE_RTCM3); //Set the UART2 to input RTCM
  }

  getPortSettings(COM_PORT_I2C); //Load the settingPayload with this port's settings
  if (settingPayload[OUTPUT_SETTING] != COM_TYPE_UBX || settingPayload[INPUT_SETTING] != COM_TYPE_UBX)
  {
    response &= myGNSS.setPortOutput(COM_PORT_I2C, COM_TYPE_UBX); //Set the I2C port to output UBX only (turn off NMEA noise)
    response &= myGNSS.setPortInput(COM_PORT_I2C, COM_TYPE_UBX); //Set the I2C port to input UBX only
  }

  //The USB port on the ZED may be used for RTCM to/from the computer (as an NTRIP caster or client)
  //So let's be sure all protocols are on for the USB port
  getPortSettings(COM_PORT_USB); //Load the settingPayload with this port's settings
  if (settingPayload[OUTPUT_SETTING] != (COM_TYPE_UBX | COM_TYPE_NMEA | COM_TYPE_RTCM3) || settingPayload[INPUT_SETTING] != (COM_TYPE_UBX | COM_TYPE_NMEA | COM_TYPE_RTCM3))
  {
    response &= myGNSS.setPortOutput(COM_PORT_USB, (COM_TYPE_UBX | COM_TYPE_NMEA | COM_TYPE_RTCM3)); //Set the USB port to everything
    response &= myGNSS.setPortInput(COM_PORT_USB, (COM_TYPE_UBX | COM_TYPE_NMEA | COM_TYPE_RTCM3)); //Set the USB port to everything
  }

  //Make sure the appropriate NMEA sentences are enabled
  response &= enableNMEASentences(COM_PORT_UART1);

  response &= myGNSS.setAutoPVT(true, false); //Tell the GPS to "send" each solution, but do not update stale data when accessed
  response &= myGNSS.setAutoHPPOSLLH(true, false); //Tell the GPS to "send" each high res solution, but do not update stale data when accessed

  if (getSerialRate(COM_PORT_UART1) != settings.dataPortBaud)
  {
    Serial.println(F("Updating UART1 rate"));
    myGNSS.setSerialRate(settings.dataPortBaud, COM_PORT_UART1); //Set UART1 to 115200
  }
  if (getSerialRate(COM_PORT_UART2) != settings.radioPortBaud)
  {
    Serial.println(F("Updating UART2 rate"));
    myGNSS.setSerialRate(settings.radioPortBaud, COM_PORT_UART2); //Set UART2 to 57600 to match SiK telemetry radio firmware default
  }

  if (response == false)
  {
    Serial.println(F("Module failed initial config."));
    return (false);
  }

  //Check system state and configure module accordingly
  if (systemState == STATE_ROVER_NO_FIX ||
      systemState == STATE_ROVER_FIX ||
      systemState == STATE_ROVER_RTK_FLOAT ||
      systemState == STATE_ROVER_RTK_FIX)
  {
    //Configure for rover mode
    if (configureUbloxModuleRover() == false)
    {
      Serial.println(F("Rover config failed!"));
      //displayRoverFail(); //Don't display here. Display error in higher level function.
      return (false);
    }
  }
  else
  {
    //Configure for base mode
    if (configureUbloxModuleBase() == false)
    {
      Serial.println(F("Base config failed!"));
      //displayBaseFail();
      return (false);
    }
  }

  response &= myGNSS.saveConfiguration(); //Save the current settings to flash and BBR
  if (response == false)
    Serial.println(F("Module failed to save."));

  return (response);
}


//Enable the NMEA sentences, based on user's settings, on a given com port
bool enableNMEASentences(uint8_t portType)
{
  bool response = true;
  if (settings.outputSentenceGGA == true)
    if (getNMEASettings(UBX_NMEA_GGA, portType) != 1)
      response &= myGNSS.enableNMEAMessage(UBX_NMEA_GGA, portType);
    else if (settings.outputSentenceGGA == false)
      if (getNMEASettings(UBX_NMEA_GGA, portType) != 0)
        response &= myGNSS.disableNMEAMessage(UBX_NMEA_GGA, portType);

  if (settings.outputSentenceGSA == true)
    if (getNMEASettings(UBX_NMEA_GSA, portType) != 1)
      response &= myGNSS.enableNMEAMessage(UBX_NMEA_GSA, portType);
    else if (settings.outputSentenceGSA == false)
      if (getNMEASettings(UBX_NMEA_GSA, portType) != 0)
        response &= myGNSS.disableNMEAMessage(UBX_NMEA_GSA, portType);

  //When receiving 15+ satellite information, the GxGSV sentences can be a large amount of data
  //If the update rate is >1Hz then this data can overcome the BT capabilities causing timeouts and lag
  //So we set the GSV sentence to 1Hz regardless of update rate
  if (settings.outputSentenceGSV == true)
    if (getNMEASettings(UBX_NMEA_GSV, portType) != settings.gnssMeasurementFrequency)
      response &= myGNSS.enableNMEAMessage(UBX_NMEA_GSV, portType, settings.gnssMeasurementFrequency);
    else if (settings.outputSentenceGSV == false)
      if (getNMEASettings(UBX_NMEA_GSV, portType) != 0)
        response &= myGNSS.disableNMEAMessage(UBX_NMEA_GSV, portType);

  if (settings.outputSentenceRMC == true)
    if (getNMEASettings(UBX_NMEA_RMC, portType) != 1)
      response &= myGNSS.enableNMEAMessage(UBX_NMEA_RMC, portType);
    else if (settings.outputSentenceRMC == false)
      if (getNMEASettings(UBX_NMEA_RMC, portType) != 0)
        response &= myGNSS.disableNMEAMessage(UBX_NMEA_RMC, portType);

  if (settings.outputSentenceGST == true)
    if (getNMEASettings(UBX_NMEA_GST, portType) != 1)
      response &= myGNSS.enableNMEAMessage(UBX_NMEA_GST, portType);
    else if (settings.outputSentenceGST == false)
      if (getNMEASettings(UBX_NMEA_GST, portType) != 0)
        response &= myGNSS.disableNMEAMessage(UBX_NMEA_GST, portType);

  return (response);
}

//Disable all the NMEA sentences on a given com port
bool disableNMEASentences(uint8_t portType)
{
  bool response = true;
  if (getNMEASettings(UBX_NMEA_GGA, portType) != 0)
    response &= myGNSS.disableNMEAMessage(UBX_NMEA_GGA, portType);
  if (getNMEASettings(UBX_NMEA_GSA, portType) != 0)
    response &= myGNSS.disableNMEAMessage(UBX_NMEA_GSA, portType);
  if (getNMEASettings(UBX_NMEA_GSV, portType) != 0)
    response &= myGNSS.disableNMEAMessage(UBX_NMEA_GSV, portType);
  if (getNMEASettings(UBX_NMEA_RMC, portType) != 0)
    response &= myGNSS.disableNMEAMessage(UBX_NMEA_RMC, portType);
  if (getNMEASettings(UBX_NMEA_GST, portType) != 0)
    response &= myGNSS.disableNMEAMessage(UBX_NMEA_GST, portType);
  if (getNMEASettings(UBX_NMEA_GLL, portType) != 0)
    response &= myGNSS.disableNMEAMessage(UBX_NMEA_GLL, portType);
  if (getNMEASettings(UBX_NMEA_VTG, portType) != 0)
    response &= myGNSS.disableNMEAMessage(UBX_NMEA_VTG, portType);

  return (response);
}

//Enable RTCM sentences for a given com port
bool enableRTCMSentences(uint8_t portType)
{
  bool response = true;
  if (getRTCMSettings(UBX_RTCM_1005, portType) != 1)
    response &= myGNSS.enableRTCMmessage(UBX_RTCM_1005, portType, 1); //Enable message 1005 to output through UART2, message every second
  if (getRTCMSettings(UBX_RTCM_1074, portType) != 1)
    response &= myGNSS.enableRTCMmessage(UBX_RTCM_1074, portType, 1);
  if (getRTCMSettings(UBX_RTCM_1084, portType) != 1)
    response &= myGNSS.enableRTCMmessage(UBX_RTCM_1084, portType, 1);
  if (getRTCMSettings(UBX_RTCM_1094, portType) != 1)
    response &= myGNSS.enableRTCMmessage(UBX_RTCM_1094, portType, 1);
  if (getRTCMSettings(UBX_RTCM_1124, portType) != 1)
    response &= myGNSS.enableRTCMmessage(UBX_RTCM_1124, portType, 1);
  if (getRTCMSettings(UBX_RTCM_1230, portType) != 10)
    response &= myGNSS.enableRTCMmessage(UBX_RTCM_1230, portType, 10); //Enable message every 10 seconds

  return (response);
}

//Disable RTCM sentences for a given com port
bool disableRTCMSentences(uint8_t portType)
{
  bool response = true;
  if (getRTCMSettings(UBX_RTCM_1005, portType) != 0)
    response &= myGNSS.disableRTCMmessage(UBX_RTCM_1005, portType);
  if (getRTCMSettings(UBX_RTCM_1074, portType) != 0)
    response &= myGNSS.disableRTCMmessage(UBX_RTCM_1074, portType);
  if (getRTCMSettings(UBX_RTCM_1084, portType) != 0)
    response &= myGNSS.disableRTCMmessage(UBX_RTCM_1084, portType);
  if (getRTCMSettings(UBX_RTCM_1094, portType) != 0)
    response &= myGNSS.disableRTCMmessage(UBX_RTCM_1094, portType);
  if (getRTCMSettings(UBX_RTCM_1124, portType) != 0)
    response &= myGNSS.disableRTCMmessage(UBX_RTCM_1124, portType);
  if (getRTCMSettings(UBX_RTCM_1230, portType) != 0)
    response &= myGNSS.disableRTCMmessage(UBX_RTCM_1230, portType);
  return (response);
}

//Given a portID, load the settings associated
bool getPortSettings(uint8_t portID)
{
  ubxPacket customCfg = {0, 0, 0, 0, 0, settingPayload, 0, 0, SFE_UBLOX_PACKET_VALIDITY_NOT_DEFINED, SFE_UBLOX_PACKET_VALIDITY_NOT_DEFINED};

  customCfg.cls = UBX_CLASS_CFG; // This is the message Class
  customCfg.id = UBX_CFG_PRT; // This is the message ID
  customCfg.len = 1;
  customCfg.startingSpot = 0; // Always set the startingSpot to zero (unless you really know what you are doing)

  uint16_t maxWait = 250; // Wait for up to 250ms (Serial may need a lot longer e.g. 1100)

  settingPayload[0] = portID; //Request the caller's portID from GPS module

  // Read the current setting. The results will be loaded into customCfg.
  if (myGNSS.sendCommand(&customCfg, maxWait) != SFE_UBLOX_STATUS_DATA_RECEIVED) // We are expecting data and an ACK
  {
    Serial.println(F("getPortSettings failed!"));
    return (false);
  }

  return (true);
}

//Given a portID and a NMEA message type, load the settings associated
uint8_t getNMEASettings(uint8_t msgID, uint8_t portID)
{
  ubxPacket customCfg = {0, 0, 0, 0, 0, settingPayload, 0, 0, SFE_UBLOX_PACKET_VALIDITY_NOT_DEFINED, SFE_UBLOX_PACKET_VALIDITY_NOT_DEFINED};

  customCfg.cls = UBX_CLASS_CFG; // This is the message Class
  customCfg.id = UBX_CFG_MSG; // This is the message ID
  customCfg.len = 2;
  customCfg.startingSpot = 0; // Always set the startingSpot to zero (unless you really know what you are doing)

  uint16_t maxWait = 250; // Wait for up to 250ms (Serial may need a lot longer e.g. 1100)

  settingPayload[0] = UBX_CLASS_NMEA;
  settingPayload[1] = msgID;

  // Read the current setting. The results will be loaded into customCfg.
  if (myGNSS.sendCommand(&customCfg, maxWait) != SFE_UBLOX_STATUS_DATA_RECEIVED) // We are expecting data and an ACK
  {
    Serial.println(F("getNMEASettings failed!"));
    return (false);
  }

  return (settingPayload[2 + portID]); //Return just the byte associated with this portID
}

//Given a portID and a RTCM message type, load the settings associated
uint8_t getRTCMSettings(uint8_t msgID, uint8_t portID)
{
  ubxPacket customCfg = {0, 0, 0, 0, 0, settingPayload, 0, 0, SFE_UBLOX_PACKET_VALIDITY_NOT_DEFINED, SFE_UBLOX_PACKET_VALIDITY_NOT_DEFINED};

  customCfg.cls = UBX_CLASS_CFG; // This is the message Class
  customCfg.id = UBX_CFG_MSG; // This is the message ID
  customCfg.len = 2;
  customCfg.startingSpot = 0; // Always set the startingSpot to zero (unless you really know what you are doing)

  uint16_t maxWait = 250; // Wait for up to 250ms (Serial may need a lot longer e.g. 1100)

  settingPayload[0] = UBX_RTCM_MSB;
  settingPayload[1] = msgID;

  // Read the current setting. The results will be loaded into customCfg.
  if (myGNSS.sendCommand(&customCfg, maxWait) != SFE_UBLOX_STATUS_DATA_RECEIVED) // We are expecting data and an ACK
  {
    Serial.println(F("getRTCMSettings failed!"));
    return (false);
  }

  return (settingPayload[2 + portID]); //Return just the byte associated with this portID
}

//Given a portID and a NMEA message type, load the settings associated
uint32_t getSerialRate(uint8_t portID)
{
  ubxPacket customCfg = {0, 0, 0, 0, 0, settingPayload, 0, 0, SFE_UBLOX_PACKET_VALIDITY_NOT_DEFINED, SFE_UBLOX_PACKET_VALIDITY_NOT_DEFINED};

  customCfg.cls = UBX_CLASS_CFG; // This is the message Class
  customCfg.id = UBX_CFG_PRT; // This is the message ID
  customCfg.len = 1;
  customCfg.startingSpot = 0; // Always set the startingSpot to zero (unless you really know what you are doing)

  uint16_t maxWait = 250; // Wait for up to 250ms (Serial may need a lot longer e.g. 1100)

  settingPayload[0] = portID;

  // Read the current setting. The results will be loaded into customCfg.
  if (myGNSS.sendCommand(&customCfg, maxWait) != SFE_UBLOX_STATUS_DATA_RECEIVED) // We are expecting data and an ACK
  {
    Serial.println(F("getSerialRate failed!"));
    return (false);
  }

  return (((uint32_t)settingPayload[10] << 16) | ((uint32_t)settingPayload[9] << 8) | settingPayload[8]);
}

boolean SFE_UBLOX_GNSS_ADD::getModuleInfo(uint16_t maxWait)
{
  myGNSS.minfo.hwVersion[0] = 0;
  myGNSS.minfo.swVersion[0] = 0;
  for (int i = 0; i < 10; i++)
    myGNSS.minfo.extension[i][0] = 0;
  myGNSS.minfo.extensionNo = 0;

  // Let's create our custom packet
  uint8_t customPayload[MAX_PAYLOAD_SIZE]; // This array holds the payload data bytes

  // The next line creates and initialises the packet information which wraps around the payload
  ubxPacket customCfg = {0, 0, 0, 0, 0, customPayload, 0, 0, SFE_UBLOX_PACKET_VALIDITY_NOT_DEFINED, SFE_UBLOX_PACKET_VALIDITY_NOT_DEFINED};

  // The structure of ubxPacket is:
  // uint8_t cls           : The message Class
  // uint8_t id            : The message ID
  // uint16_t len          : Length of the payload. Does not include cls, id, or checksum bytes
  // uint16_t counter      : Keeps track of number of overall bytes received. Some responses are larger than 255 bytes.
  // uint16_t startingSpot : The counter value needed to go past before we begin recording into payload array
  // uint8_t *payload      : The payload
  // uint8_t checksumA     : Given to us by the module. Checked against the rolling calculated A/B checksums.
  // uint8_t checksumB
  // sfe_ublox_packet_validity_e valid            : Goes from NOT_DEFINED to VALID or NOT_VALID when checksum is checked
  // sfe_ublox_packet_validity_e classAndIDmatch  : Goes from NOT_DEFINED to VALID or NOT_VALID when the Class and ID match the requestedClass and requestedID

  // sendCommand will return:
  // SFE_UBLOX_STATUS_DATA_RECEIVED if the data we requested was read / polled successfully
  // SFE_UBLOX_STATUS_DATA_SENT     if the data we sent was writted successfully (ACK'd)
  // Other values indicate errors. Please see the sfe_ublox_status_e enum for further details.

  // Referring to the u-blox M8 Receiver Description and Protocol Specification we see that
  // the module information can be read using the UBX-MON-VER message. So let's load our
  // custom packet with the correct information so we can read (poll / get) the module information.

  customCfg.cls = UBX_CLASS_MON; // This is the message Class
  customCfg.id = UBX_MON_VER;    // This is the message ID
  customCfg.len = 0;             // Setting the len (length) to zero let's us poll the current settings
  customCfg.startingSpot = 0;    // Always set the startingSpot to zero (unless you really know what you are doing)

  // Now let's send the command. The module info is returned in customPayload

  if (sendCommand(&customCfg, maxWait) != SFE_UBLOX_STATUS_DATA_RECEIVED)
    return (false); //If command send fails then bail

  // Now let's extract the module info from customPayload

  uint16_t position = 0;
  for (int i = 0; i < 30; i++)
  {
    minfo.swVersion[i] = customPayload[position];
    position++;
  }
  for (int i = 0; i < 10; i++)
  {
    minfo.hwVersion[i] = customPayload[position];
    position++;
  }

  while (customCfg.len >= position + 30)
  {
    for (int i = 0; i < 30; i++)
    {
      minfo.extension[minfo.extensionNo][i] = customPayload[position];
      position++;
    }
    minfo.extensionNo++;
    if (minfo.extensionNo > 9)
      break;
  }

  return (true); //Success!
}

//Call back for when BT connection event happens (connected/disconnect)
//Used for updating the radioState state machine
void btCallback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
  if (event == ESP_SPP_SRV_OPEN_EVT) {
    Serial.println(F("Client Connected"));
    radioState = BT_CONNECTED;
  }

  if (event == ESP_SPP_CLOSE_EVT ) {
    Serial.println(F("Client disconnected"));
    radioState = BT_ON_NOCONNECTION;
  }
}

//When called, checks level of battery and updates the LED brightnesses
//And outputs a serial message to USB
void checkBatteryLevels()
{
  if (millis() - lastBattUpdate > 1000)
  {
    lastBattUpdate = millis();

    battLevel = lipo.getSOC();
    battVoltage = lipo.getVoltage();
    battChangeRate = lipo.getChangeRate();

    Serial.printf("Batt (%d%%): Voltage: %0.02fV", battLevel, battVoltage);

    char tempStr[25];
    if (battChangeRate > 0)
      sprintf(tempStr, "C");
    else
      sprintf(tempStr, "Disc");
    Serial.printf(" %sharging: %0.02f%%/hr ", tempStr, battChangeRate);

    if (battLevel < 10)
    {
      sprintf(tempStr, "RED uh oh!");
    }
    else if (battLevel < 50)
    {
      sprintf(tempStr, "Yellow ok");
    }
    else if (battLevel >= 50)
    {
      sprintf(tempStr, "Green all good");
    }
    else
    {
      sprintf(tempStr, "No batt");
    }

    Serial.printf("%s\n", tempStr);
  }
}

//Ping an I2C device and see if it responds
bool isConnected(uint8_t deviceAddress)
{
  Wire.beginTransmission(deviceAddress);
  if (Wire.endTransmission() == 0)
    return true;
  return false;
}

//Given text, a position, and kerning, print text to display
//This is helpful for squishing or stretching a string to appropriately fill the display
void printTextwithKerning(char *newText, uint8_t xPos, uint8_t yPos, uint8_t kerning)
{
  for (int x = 0 ; x < strlen(newText) ; x++)
  {
    oled.setCursor(xPos, yPos);
    oled.print(newText[x]);
    xPos += kerning;
  }
}