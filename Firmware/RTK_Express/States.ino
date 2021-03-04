/*
  This is the main state machine for the device. It's big but controls each step of the system.
  See system state chart document for a visual representation of how states can change to/from.
*/

//Given the current state, see if conditions have moved us to a new state
//A user pressing the setup button (change between rover/base) is handled by checkSetupButton()
void updateSystemState()
{
  if (millis() - lastSystemStateUpdate > 500)
  {
    lastSystemStateUpdate = millis();

    //Move between states as needed
    switch (systemState)
    {
      case (STATE_ROVER_NO_FIX):
        {
          if (myGNSS.getFixType() == 3) //3D
            systemState = STATE_ROVER_FIX;
        }
        break;

      case (STATE_ROVER_FIX):
        {
          byte rtkType = myGNSS.getCarrierSolutionType();
          if (rtkType == 1) //RTK Float
            systemState = STATE_ROVER_RTK_FLOAT;
          else if (rtkType == 2) //RTK Fix
            systemState = STATE_ROVER_RTK_FIX;
        }
        break;

      case (STATE_ROVER_RTK_FLOAT):
        {
          byte rtkType = myGNSS.getCarrierSolutionType();
          if (rtkType == 0) //No RTK
            systemState = STATE_ROVER_FIX;
          if (rtkType == 2) //RTK Fix
            systemState = STATE_ROVER_RTK_FIX;
        }
        break;

      case (STATE_ROVER_RTK_FIX):
        {
          byte rtkType = myGNSS.getCarrierSolutionType();
          if (rtkType == 0) //No RTK
            systemState = STATE_ROVER_FIX;
          if (rtkType == 1) //RTK Float
            systemState = STATE_ROVER_RTK_FLOAT;
        }
        break;

      //Wait for horz acc of 5m or less before starting survey in
      case (STATE_BASE_TEMP_SURVEY_NOT_STARTED):
        {
          //Check for <5m horz accuracy
          uint32_t accuracy = myGNSS.getHorizontalAccuracy();

          float f_accuracy = accuracy;
          f_accuracy = f_accuracy / 10000.0; // Convert the horizontal accuracy (mm * 10^-1) to a float

          Serial.printf("Waiting for Horz Accuracy < %0.2f meters: %0.2f\n", settings.surveyInStartingAccuracy, f_accuracy);

          if (f_accuracy > 0.0 && f_accuracy < settings.surveyInStartingAccuracy)
          {
            displaySurveyStart(); //Show 'Survey'
            if (configureUbloxModuleBase() == true)
            {
              if (surveyIn() == true) //Begin survey
              {
                displaySurveyStarted(); //Show 'Survey Started'
                systemState = STATE_BASE_TEMP_SURVEY_STARTED;
              }
            }
          }
        }
        break;

      //Check survey status until it completes or 15 minutes elapses and we go back to rover
      case (STATE_BASE_TEMP_SURVEY_STARTED):
        {
          if (myGNSS.getFixType() == 5) //We have a TIME fix which is survey in complete
          {
            Serial.println(F("Base survey complete! RTCM now broadcasting."));
            systemState = STATE_BASE_TEMP_TRANSMITTING;
          }
          else
          {
            Serial.print(F("Time elapsed: "));
            Serial.print((String)myGNSS.getSurveyInObservationTime());
            Serial.print(F(" Accuracy: "));
            Serial.print((String)myGNSS.getSurveyInMeanAccuracy());
            Serial.print(F(" SIV: "));
            Serial.print(myGNSS.getSIV());
            Serial.println();

            if (myGNSS.getSurveyInObservationTime() > maxSurveyInWait_s)
            {
              Serial.printf("Survey-In took more than %d minutes. Returning to rover mode.\n", maxSurveyInWait_s / 60);

              resetSurvey();

              displayRoverStart();

              //Configure for rover mode
              Serial.println(F("Rover Mode"));

              //If we are survey'd in, but switch is rover then disable survey
              if (configureUbloxModuleRover() == false)
              {
                Serial.println(F("Rover config failed!"));
                displayRoverFail();
                return;
              }

              beginBluetooth(); //Restart Bluetooth with 'Rover' name

              systemState = STATE_ROVER_NO_FIX;
              displayRoverSuccess();
            }
          }
        }
        break;

      //Leave base temp transmitting if user has enabled WiFi/NTRIP
      case (STATE_BASE_TEMP_TRANSMITTING):
        {
          if (settings.enableNtripServer == true)
          {
            //Turn off Bluetooth and turn on WiFi
            endBluetooth();

            Serial.printf("Connecting to local WiFi: %s\n", settings.wifiSSID);
            WiFi.begin(settings.wifiSSID, settings.wifiPW);

            radioState = WIFI_ON_NOCONNECTION;
            systemState = STATE_BASE_TEMP_WIFI_STARTED;
          }
        }
        break;

      //Check to see if we have connected over WiFi
      case (STATE_BASE_TEMP_WIFI_STARTED):
        {
          byte wifiStatus = WiFi.status();
          if (wifiStatus == WL_CONNECTED)
          {
            radioState = WIFI_CONNECTED;

            systemState = STATE_BASE_TEMP_WIFI_CONNECTED;
          }
          else
          {
            Serial.print(F("WiFi Status: "));
            switch (wifiStatus) {
              case WL_NO_SHIELD: Serial.println(F("WL_NO_SHIELD"));
              case WL_IDLE_STATUS: Serial.println(F("WL_IDLE_STATUS"));
              case WL_NO_SSID_AVAIL: Serial.println(F("WL_NO_SSID_AVAIL"));
              case WL_SCAN_COMPLETED: Serial.println(F("WL_SCAN_COMPLETED"));
              case WL_CONNECTED: Serial.println(F("WL_CONNECTED"));
              case WL_CONNECT_FAILED: Serial.println(F("WL_CONNECT_FAILED"));
              case WL_CONNECTION_LOST: Serial.println(F("WL_CONNECTION_LOST"));
              case WL_DISCONNECTED: Serial.println(F("WL_DISCONNECTED"));
            }
          }
        }
        break;

      case (STATE_BASE_TEMP_WIFI_CONNECTED):
        {
          if (settings.enableNtripServer == true)
          {
            //Open connection to caster service
            if (caster.connect(settings.casterHost, settings.casterPort) == true) //Attempt connection
            {
              systemState = STATE_BASE_TEMP_CASTER_STARTED;

              Serial.printf("Connected to %s:%d\n", settings.casterHost, settings.casterPort);

              const int SERVER_BUFFER_SIZE  = 512;
              char serverBuffer[SERVER_BUFFER_SIZE];

              snprintf(serverBuffer, SERVER_BUFFER_SIZE, "SOURCE %s /%s\r\nSource-Agent: NTRIP %s/%s\r\n\r\n",
                       settings.mountPointPW, settings.mountPoint, ntrip_server_name, "App Version 1.0");

              Serial.printf("Sending credentials:\n%s\n", serverBuffer);
              caster.write(serverBuffer, strlen(serverBuffer));

              casterResponseWaitStartTime = millis();
            }
          }
        }
        break;

      //Wait for response for caster service and make sure it's valid
      case (STATE_BASE_TEMP_CASTER_STARTED):
        {
          //Check if caster service responded
          if (caster.available() == 0)
          {
            if (millis() - casterResponseWaitStartTime > 5000)
            {
              Serial.println(F("Caster failed to respond. Do you have your caster address and port correct?"));
              caster.stop();
              delay(10); //Yield to RTOS

              systemState = STATE_BASE_TEMP_WIFI_CONNECTED; //Return to previous state
            }
          }
          else
          {
            //Check reply
            bool connectionSuccess = false;
            char response[512];
            int responseSpot = 0;
            while (caster.available())
            {
              response[responseSpot++] = caster.read();
              if (strstr(response, "200") > 0) //Look for 'ICY 200 OK'
                connectionSuccess = true;
              if (responseSpot == 512 - 1) break;
            }
            response[responseSpot] = '\0';
            Serial.printf("Caster responded with: %s\n", response);

            if (connectionSuccess == false)
            {
              Serial.printf("Caster responded with bad news: %s. Are you sure your caster credentials are correct?", response);
              systemState = STATE_BASE_TEMP_WIFI_CONNECTED; //Return to previous state
            }
            else
            {
              //We're connected!
              Serial.println(F("Connected to caster"));

              //Reset flags
              lastServerReport_ms = millis();
              lastServerSent_ms = millis();
              serverBytesSent = 0;

              systemState = STATE_BASE_TEMP_CASTER_CONNECTED;
            }
          }
        }
        break;

      //Monitor connected state
      case (STATE_BASE_TEMP_CASTER_CONNECTED):
        {
          if (caster.connected() == false)
          {
            systemState = STATE_BASE_TEMP_WIFI_CONNECTED; //Return to 2 earlier states to try to reconnect
          }
        }
        break;

      //Leave base fixed transmitting if user has enabled WiFi/NTRIP
      case (STATE_BASE_FIXED_TRANSMITTING):
        {
          if (settings.enableNtripServer == true)
          {
            //Turn off Bluetooth and turn on WiFi
            endBluetooth();

            Serial.printf("Connecting to local WiFi: %s", settings.wifiSSID);
            WiFi.begin(settings.wifiSSID, settings.wifiPW);

            radioState = WIFI_ON_NOCONNECTION;
            systemState = STATE_BASE_FIXED_WIFI_STARTED;
          }
        }
        break;

      //Check to see if we have connected over WiFi
      case (STATE_BASE_FIXED_WIFI_STARTED):
        {
          byte wifiStatus = WiFi.status();
          if (wifiStatus == WL_CONNECTED)
          {
            radioState = WIFI_CONNECTED;

            systemState = STATE_BASE_FIXED_WIFI_CONNECTED;
          }
          else
          {
            Serial.print(F("WiFi Status: "));
            switch (wifiStatus) {
              case WL_NO_SHIELD: Serial.println(F("WL_NO_SHIELD")); break;
              case WL_IDLE_STATUS: Serial.println(F("WL_IDLE_STATUS")); break;
              case WL_NO_SSID_AVAIL: Serial.println(F("WL_NO_SSID_AVAIL")); break;
              case WL_SCAN_COMPLETED: Serial.println(F("WL_SCAN_COMPLETED")); break;
              case WL_CONNECTED: Serial.println(F("WL_CONNECTED")); break;
              case WL_CONNECT_FAILED: Serial.println(F("WL_CONNECT_FAILED")); break;
              case WL_CONNECTION_LOST: Serial.println(F("WL_CONNECTION_LOST")); break;
              case WL_DISCONNECTED: Serial.println(F("WL_DISCONNECTED")); break;
            }
          }
        }
        break;

      case (STATE_BASE_FIXED_WIFI_CONNECTED):
        {
          if (settings.enableNtripServer == true)
          {
            //Open connection to caster service
            if (caster.connect(settings.casterHost, settings.casterPort) == true) //Attempt connection
            {
              systemState = STATE_BASE_FIXED_CASTER_STARTED;

              Serial.printf("Connected to %s:%d\n", settings.casterHost, settings.casterPort);

              const int SERVER_BUFFER_SIZE  = 512;
              char serverBuffer[SERVER_BUFFER_SIZE];

              snprintf(serverBuffer, SERVER_BUFFER_SIZE, "SOURCE %s /%s\r\nSource-Agent: NTRIP %s/%s\r\n\r\n",
                       settings.mountPointPW, settings.mountPoint, ntrip_server_name, "App Version 1.0");

              Serial.printf("Sending credentials:\n%s\n", serverBuffer);
              caster.write(serverBuffer, strlen(serverBuffer));

              casterResponseWaitStartTime = millis();
            }
          }
        }
        break;

      //Wait for response for caster service and make sure it's valid
      case (STATE_BASE_FIXED_CASTER_STARTED):
        {
          //Check if caster service responded
          if (caster.available() < 10)
          {
            if (millis() - casterResponseWaitStartTime > 5000)
            {
              Serial.println(F("Caster failed to respond. Do you have your caster address and port correct?"));
              caster.stop();
              delay(10); //Yield to RTOS

              systemState = STATE_BASE_FIXED_WIFI_CONNECTED; //Return to previous state
            }
          }
          else
          {
            //Check reply
            bool connectionSuccess = false;
            char response[512];
            int responseSpot = 0;
            while (caster.available())
            {
              response[responseSpot++] = caster.read();
              if (strstr(response, "200") > 0) //Look for 'ICY 200 OK'
                connectionSuccess = true;
              if (responseSpot == 512 - 1) break;
            }
            response[responseSpot] = '\0';
            Serial.printf("Caster responded with: %s\n", response);

            if (connectionSuccess == false)
            {
              Serial.printf("Caster responded with bad news: %s. Are you sure your caster credentials are correct?", response);
              systemState = STATE_BASE_FIXED_WIFI_CONNECTED; //Return to previous state
            }
            else
            {
              //We're connected!
              Serial.println(F("Connected to caster"));

              //Reset flags
              lastServerReport_ms = millis();
              lastServerSent_ms = millis();
              serverBytesSent = 0;

              systemState = STATE_BASE_FIXED_CASTER_CONNECTED;
            }
          }
        }
        break;

      //Monitor connected state
      case (STATE_BASE_FIXED_CASTER_CONNECTED):
        {
          if (caster.connected() == false)
          {
            systemState = STATE_BASE_FIXED_WIFI_CONNECTED; //Return to 2 earlier states to try to reconnect
          }
        }
        break;

    }

    //Debug print
    switch (systemState)
    {
      case (STATE_ROVER_NO_FIX):
        Serial.println(F("State: Rover - No Fix"));
        break;
      case (STATE_ROVER_FIX):
        Serial.println(F("State: Rover - Fix"));
        break;
      case (STATE_ROVER_RTK_FLOAT):
        Serial.println(F("State: Rover - RTK Float"));
        break;
      case (STATE_ROVER_RTK_FIX):
        Serial.println(F("State: Rover - RTK Fix"));
        break;
      case (STATE_BASE_TEMP_SURVEY_NOT_STARTED):
        Serial.println(F("State: Base-Temp - Survey Not Started"));
        break;
      case (STATE_BASE_TEMP_SURVEY_STARTED):
        Serial.println(F("State: Base-Temp - Survey Started"));
        break;
      case (STATE_BASE_TEMP_TRANSMITTING):
        Serial.println(F("State: Base-Temp - Transmitting"));
        break;
      case (STATE_BASE_TEMP_WIFI_STARTED):
        Serial.println(F("State: Base-Temp - WiFi Started"));
        break;
      case (STATE_BASE_TEMP_WIFI_CONNECTED):
        Serial.println(F("State: Base-Temp - WiFi Connected"));
        break;
      case (STATE_BASE_TEMP_CASTER_STARTED):
        Serial.println(F("State: Base-Temp - Caster Started"));
        break;
      case (STATE_BASE_TEMP_CASTER_CONNECTED):
        Serial.println(F("State: Base-Temp - Caster Connected"));
        break;
      case (STATE_BASE_FIXED_TRANSMITTING):
        Serial.println(F("State: Base-Fixed - Transmitting"));
        break;
      case (STATE_BASE_FIXED_WIFI_STARTED):
        Serial.println(F("State: Base-Fixed - WiFi Started"));
        break;
      case (STATE_BASE_FIXED_WIFI_CONNECTED):
        Serial.println(F("State: Base-Fixed - WiFi Connected"));
        break;
      case (STATE_BASE_FIXED_CASTER_STARTED):
        Serial.println(F("State: Base-Fixed - Caster Started"));
        break;
      case (STATE_BASE_FIXED_CASTER_CONNECTED):
        Serial.println(F("State: Base-Fixed - Caster Connected"));
        break;
      default:
        Serial.printf("State Unknown: %d\n", systemState);
        break;
    }
  }
}
