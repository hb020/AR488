#include <Arduino.h>
//#include <SD.h>
#include "AR488_Config.h"
#include "AR488_GPIBbus.h"

/***** AR488_GPIB.cpp, ver. 0.53.11, 08/05/2025 *****/


/****** Process status values *****/
#define OK false
#define ERR true

/***** Control characters *****/
#define ESC 0x1B   // the USB escape char
#define CR 0xD     // Carriage return
#define LF 0xA     // Newline/linefeed
#define PLUS 0x2B  // '+' character



/***************************************/
/***** GPIB CLASS PUBLIC FUNCTIONS *****/
/***** vvvvvvvvvvvvvvvvvvvvvvvvvvv *****/


/********** PUBLIC FUNCTIONS **********/

/***** Class constructor *****/
GPIBbus::GPIBbus() {
  // Default configuration values
  setDefaultCfg();
  cstate = 0;
  deviceAddressed = TONONE;
}


/***** Start the bus in controller or device mode depending on config *****/
void GPIBbus::begin() {
#if defined(RAS_PICO_L1) || defined(RAS_PICO_L2) 
  initRpGpioPins();
#endif
//gpioFuncList();
#ifdef LEVEL_SHIFTER
  initLevelShifter();
#endif
  if (isController()) {
    startControllerMode();
//    gpioFuncList();
  } else {
    startDeviceMode();
  }
}


/***** Stops active mode and bring control and data bus to inactive state *****/
void GPIBbus::stop() {
  cstate = 0;
  // Set control bus to idle state (all lines input_pullup)
//Serial.println(F("Clear all signals to input pullup"));
  clearAllSignals();
//  gpioFuncList();
  // Set data bus to default state (all lines input_pullup)
//Serial.println(F("Ready the data bus:"));
  readyGpibDbus();
//  gpioFuncList();
#ifdef LEVEL_SHIFTER
  // Disable level shifter
  shiftEnable(false);
#endif
}


/***** Initialise the interface *****/
void GPIBbus::setDefaultCfg() {
  // Set default controller mode values ({'\0'} sets version string array to null)
  cfg = { false, false, 2, 0, 1, 0xFF, 0, 0, 0, 1200, 0, { '\0' }, 0, { '\0' }, 0, 0, 0 };
}


/***** Set bus into Device mode *****/
void GPIBbus::startDeviceMode() {
  // Stop current mode
  stop();
  delayMicroseconds(200);  // Allow settling time
  // Start device mode
  cfg.cmode = 1;
  // Set GPIB control bus to device idle mode
  setControls(DINI);
  // Initialise GPIB data lines (sets to INPUT_PULLUP)
//  readyGpibDbus();
#ifdef LEVEL_SHIFTER
  // Enable level shifter
  shiftEnable(true);
#endif
}


/***** Set interface into Controller mode *****/
void GPIBbus::startControllerMode() {
  // Send request to clear all devices on bus to local mode
  sendAllClear();
  // Stop current mode
  stop();
  delayMicroseconds(200);  // Allow settling time
  // Start controller mode
  cfg.cmode = 2;
  // Set GPIB control bus to controller idle mode
  setControls(CINI);
  // Initialise GPIB data lines (sets to INPUT_PULLUP)
  readyGpibDbus();
#ifdef LEVEL_SHIFTER
  // Enable level shifter
  shiftEnable(true);
  delay(100);
#endif
  // Assert IFC to signal controller in charge (CIC)
  sendIFC();
}


/***** Set the interface mode *****/
void GPIBbus::setOperatingMode(enum operatingMode mode) {
  uint8_t outputs = 0;
  switch (mode) {
    case OP_IDLE:
      setGpibCtrlDir(0, CTRL_BITS);             // Set all control signals to input_pullup
      break;
    case OP_CTRL:
      outputs = (IFC_BIT | REN_BIT | ATN_BIT);  // Signal IFC, REN and ATN, listen to SRQ
      setGpibCtrlDir(outputs, CTRL_BITS);       // Set control inputs and outputs (0=input_pullup, 1=output)
      setGpibCtrlState(outputs, outputs);       // Set control output signals to unasserted/HIGH
      break;
    case OP_DEVI:
      outputs = (SRQ_BIT);                      // Signal SRQ, listen to IFC, REN and ATN
      clearSignal(REN_BIT);
      setGpibCtrlDir(outputs, CTRL_BITS);       // Set control inputs and outputs (0=input_pullup, 1=output)
      setGpibCtrlState(outputs, outputs);       // Set control output signals to unasserted/HIGH
      break;
  }
}


/***** Set the transmission mode *****/
void GPIBbus::setTransmitMode(enum transmitMode mode) {
  uint8_t outputs = 0;
  switch (mode) {
    case TM_IDLE:
      setGpibCtrlDir(0, HSHK_BITS);           // Set all handshake signals to input_pullup
      break;
    case TM_RECV:
      outputs = (NRFD_BIT | NDAC_BIT);        // Signal NRFD and NDAC, listen to DAV and EOI
      setGpibCtrlDir(outputs, HSHK_BITS);     // Set handshake inputs and outputs (0=input_PULLUP, 1=output)
      setGpibCtrlState(~outputs, outputs);    // Set handshake output signals to asserted/LOW
      break;
    case TM_SEND:
      outputs = (DAV_BIT | EOI_BIT);          // Signal DAV and EOI, listen to NRFD and NDAC
      setGpibCtrlDir(outputs, HSHK_BITS);     // Set handshake inputs and outputs (0=input_pullup, 1=output)
      setGpibCtrlState(outputs, outputs);     // Set handshake output signals to unasserted/HIGH
      break;
  }
}


/***** Assert an individual or group of signals *****/
void GPIBbus::assertSignal(uint8_t sig) {
  // Note: GPIO pin direction assumed set by setOperatingMode()
  setGpibCtrlState(0, sig);   // Set all signals permitted by mask to LOW (asserted)
}


/***** Clear (unassert) an individual or group of signals *****/
void GPIBbus::clearSignal(uint8_t sig) {
  // Note: GPIO pin direction assumed set by setOperatingMode()
  setGpibCtrlState(sig, sig);   // Set all signals permitted by mask to HIGH (unasserted)
}


/***** Clear all GPIB control signals *****/
void GPIBbus::clearAllSignals() {
  setGpibCtrlDir(0, ALL_BITS);            // Set all control signal pins to input_pullup
}


/***** Return current cinterface mode *****/
bool GPIBbus::isController() {
  if (cfg.cmode == 2) return true;
  return false;
}


/***** Detect selected pin state *****/
bool GPIBbus::isAsserted(uint8_t gpibsig) {
#ifdef AR488_MCP23S17 
  uint8_t mcpPinAssertedReg = 0;
  mcpPinAssertedReg = ~getMcpIntAReg();
  return (mcpPinAssertedReg & (1 << gpibsig));
#else
  // Use digitalRead function to get current Arduino pin state
  return (digitalRead(gpibsig) == LOW) ? true : false;
#endif
  if (getGpibPinState(gpibsig) == LOW) return true;
  return false;
}


/***** Send the device status byte *****/
void GPIBbus::sendStatus() {
  // Have been addressed and polled so send the status byte
  if (!(cstate == DTAS)) setControls(DTAS);
  writeByte(cfg.stat, NO_EOI);
  setControls(DIDS);
  // Clear the SRQ bit
  cfg.stat = cfg.stat & ~0x40;
  // De-assert the SRQ signal
  clearSignal(SRQ_BIT);
}


/***** Set the status byte *****/
void GPIBbus::setStatus(uint8_t statusByte) {
  cfg.stat = statusByte;
  if (statusByte & 0x40) {
    // If SRQ bit is set then assert the SRQ signal
    assertSignal(SRQ_BIT);
  } else {
    // If SRQ bit is NOT set then de-assert the SRQ signal
    clearSignal(SRQ_BIT);
  }
}


/***** Send IFC *****/
void GPIBbus::sendIFC() {
  // Assert IFC
  assertSignal(IFC_BIT);
  delayMicroseconds(150);
  // De-assert IFC
  clearSignal(IFC_BIT);
}


/***** Send SDC GPIB command *****/
bool GPIBbus::sendSDC() {
#ifdef DEBUG_GPIB_COMMANDS
  DB_PRINT(F("sending SDC..."), "");
#endif
  if (addressDevice(cfg.paddr, cfg.saddr, TOLISTEN)) {
#ifdef DEBUG_GPIB_COMMANDS
    DB_PRINT(F("failed to address the device."), "");
#endif
    return ERR;
  }
  // Send SDC to currently addressed device
  if (sendCmd(GC_SDC)) {
#ifdef DEBUG_GPIB_COMMANDS
    DB_PRINT(F("failed to send SDC to device"), "");
#endif
    return ERR;
  }
  // Unlisten bus
  if (unAddressDevice()) {
#ifdef DEBUG_GPIB_COMMANDS
    DB_PRINT(F("failed to unlisten the GPIB bus"), "");
#endif
    return ERR;
  }
#ifdef DEBUG_GPIB_COMMANDS
  DB_PRINT(F("done."), "");
#endif
  return OK;
}


/***** Send LLO GPIB command *****/
bool GPIBbus::sendLLO() {
#ifdef DEBUG_GPIB_COMMANDS
  DB_PRINT(F("sending LLO..."), "");
#endif
  if (addressDevice(cfg.paddr, cfg.saddr, TOLISTEN)) {
#ifdef DEBUG_GPIB_COMMANDS
    DB_PRINT(F("failed to address the device."), "");
#endif
    return ERR;
  }
  // Send LLO to currently addressed device
  if (sendCmd(GC_LLO)) {
#ifdef DEBUG_GPIB_COMMANDS
    DB_PRINT(F("failed to send LLO to device"), "");
#endif
    return ERR;
  }
  // Unlisten bus
  if (unAddressDevice()) {
#ifdef DEBUG_GPIB_COMMANDS
    DB_PRINT(F("failed to unlisten the GPIB bus"), "");
#endif
    return ERR;
  }
#ifdef DEBUG_GPIB_COMMANDS
  DB_PRINT(F("done."), "");
#endif
  return OK;
}


/***** Send LOC GPIB command *****/
bool GPIBbus::sendGTL() {
#ifdef DEBUG_GPIB_COMMANDS
  DB_PRINT(F("sending LOC..."), "");
#endif
  if (addressDevice(cfg.paddr, cfg.saddr, TOLISTEN)) {
#ifdef DEBUG_GPIB_COMMANDS
    DB_PRINT(F("failed to address the device."), "");
#endif
    return ERR;
  }
  // Send GTL
  if (sendCmd(GC_GTL)) {
#ifdef DEBUG_GPIB_COMMANDS
    DB_PRINT(F("failed to send LOC to device"), "");
#endif
    return ERR;
  }
  // Unlisten bus
  if (unAddressDevice()) {
#ifdef DEBUG_GPIB_COMMANDS
    DB_PRINT(F("failed to unlisten the GPIB bus"), "");
#endif
    return ERR;
  }
#ifdef DEBUG_GPIB_COMMANDS
  DB_PRINT(F("done."), "");
#endif
  return OK;
}


/***** Send GET (trigger) command *****/
bool GPIBbus::sendGET(uint8_t addr) {
#ifdef DEBUG_GPIB_COMMANDS
  DB_PRINT(F("sending GET..."), "");
#endif
  if (addressDevice(addr, 0xFF, TOLISTEN)) {
#ifdef DEBUG_GPIB_COMMANDS
    DB_PRINT(F("failed to address the device."), "");
#endif
    return ERR;
  }
  // Send GET
  if (sendCmd(GC_GET)) {
#ifdef DEBUG_GPIB_COMMANDS
    DB_PRINT(F("failed to send GET to device"), "");
#endif
    return ERR;
  }
  // Unlisten bus
  if (unAddressDevice()) {
#ifdef DEBUG_GPIB_COMMANDS
    DB_PRINT(F("failed to unlisten the GPIB bus"), "");
#endif
    return ERR;
  }
#ifdef DEBUG_GPIB_COMMANDS
  DB_PRINT(F("done."), "");
#endif
  return OK;
}


/***** Send a TCT (Take Control) command *****/
bool GPIBbus::sendTCT(uint8_t addr){
 #ifdef DEBUG_GPIB_COMMANDS
  DB_PRINT(F("sending TCT..."), "");
#endif
  if (addressDevice(addr, 0xFF, TOLISTEN)) {
#ifdef DEBUG_GPIB_COMMANDS
    DB_PRINT(F("failed to address the device."), "");
#endif
    return ERR;
  }
  // Send TCT
  if (sendCmd(GC_TCT)) {
#ifdef DEBUG_GPIB_COMMANDS
    DB_PRINT(F("failed to send TCT to device"), "");
#endif
    return ERR;
  }
  // Unlisten bus
  if (unAddressDevice()) {
#ifdef DEBUG_GPIB_COMMANDS
    DB_PRINT(F("failed to unlisten the GPIB bus"), "");
#endif
    return ERR;
  }
#ifdef DEBUG_GPIB_COMMANDS
  DB_PRINT(F("done."), "");
#endif
  return OK;
}


/***** Send request to clear to all devices to local *****/
void GPIBbus::sendAllClear() {
  // Un-assert REN
  clearSignal(REN_BIT);
  delay(40);
  // Simultaneously assert ATN and REN
  assertSignal(ATN_BIT | REN_BIT);
  delay(40);
  // Unassert ATN
  clearSignal(ATN_BIT);
}


/***** Request device to talk *****/
/*
bool GPIBbus::sendMTA() {
#ifdef DEBUG_GPIB_ADDRESSING
  DB_PRINT(F("address device: "), cfg.paddr);
#endif
  if (cstate != CCMS) setControls(CCMS);
#ifdef DEBUG_GPIB_ADDRESSING
  DB_PRINT(F("cstate: "),hexstr);
#endif
  if (addressDevice(cfg.paddr, 1)) return ERR;
#ifdef DEBUG_GPIB_ADDRESSING
  DB_PRINT(F("done."), "");
#endif
  return OK;
}
*/

/***** Request device to listen *****/
/*
bool GPIBbus::sendMLA() {
#ifdef DEBUG_GPIB_ADDRESSING
  DB_PRINT(F("address device: "), cfg.paddr);
#endif
  if (cstate != CCMS) setControls(CCMS);
#ifdef DEBUG_GPIB_ADDRESSING
  DB_PRINT(F("cstate: "),hexstr);
#endif
  if (addressDevice(cfg.paddr, 0)) return ERR;
#ifdef DEBUG_GPIB_ADDRESSING
  DB_PRINT(F("done."), "");
#endif
  return OK;
}
*/

/***** Send secondary address command *****/
/*
bool GPIBbus::sendMSA(uint8_t addr) {
  // Send address
  if (sendCmd(addr)) {
#ifdef DEBUG_GPIB_COMMANDS
    DB_PRINT(F("failed to send MSA to device"), "");
#endif
    return ERR;
  }
  // Unassert ATN
  clearSignal(ATN_BIT);
  return OK;
}
*/

/***** Send untalk *****/
bool GPIBbus::sendUNT() {
  if (sendCmd(GC_UNT)) {
#ifdef DEBUG_GPIB_ADDRESSING
    DB_PRINT(F("failed to send UNT to device"), "");
#endif
    return ERR;
  }
//  setControls(CIDS);
  deviceAddressed = TONONE;
  return OK;
}


/***** Send unlisten *****/
bool GPIBbus::sendUNL() {
  if (sendCmd(GC_UNL)) {
#ifdef DEBUG_GPIB_ADDRESSING
    DB_PRINT(F("failed to send UNL to device"), "");
#endif
    return ERR;
  }
//  setControls(CIDS);
  deviceAddressed = TONONE;
  return OK;
}


/*****  Send a single byte GPIB command *****/
bool GPIBbus::sendCmd(uint8_t cmdByte) {
  enum gpibHandshakeState state;

  // Set lines for command and assert ATN
  if (cstate != CCMS) setControls(CCMS);
  // Send the command
  state = writeByte(cmdByte, NO_EOI);
  if (state == HANDSHAKE_COMPLETE) return OK;

#if defined(DEBUG_GPIBbus_RECEIVE) || defined(DEBUG_GPIBbus_SEND)
  char buffer[40];
  sprintf(buffer, "Failed to send command %02X to device ", cmdByte);
  DB_PRINT(buffer, cfg.paddr);
#endif

  return ERR;
}


/***** Receive data from the GPIB bus ****/
/*
 * Readbreak:
 * 7 - command received via serial
 */
enum receiveState GPIBbus::receiveData(Stream &dataStream, bool detectEoi, bool detectEndByte, uint8_t endByte, int maxSize) {

  uint8_t bytes[3] = { 0 };  // Received byte buffer
  uint8_t eor = cfg.eor & 7;
  int x = 0;
  bool readWithEoi = false;
  bool eoiDetected = false;
  enum gpibHandshakeState hstate = HANDSHAKE_COMPLETE;
  enum receiveState rstate = RECEIVE_INIT;

  if (cfg.eot_en && maxSize > 0) maxSize--;  // EOT character might get added to the end of the string
  endByte = endByte;  // meaningless but defeats vcompiler warning!

  // Reset transmission break flag
  txBreak = false;

  // EOI detection required ?
  if (cfg.eoi || detectEoi || (cfg.eor == 7)) readWithEoi = true;  // Use EOI as terminator

  // Set up for reading in Controller mode
  if (cfg.cmode == 2) {  // Controler mode

/*
    // Address device to talk
    if (addressDevice(cfg.paddr, cfg.saddr, TOTALK)) {
#ifdef DEBUG_GPIBbus_RECEIVE
      DB_PRINT(F("Failed to address device to talk: "), cfg.paddr);
#endif
    }
*/
    // Wait for instrument ready
    // Set GPIB control lines to controller read mode
    setControls(CLAS);

    // Set up for reading in Device mode
  } else {  // Device mode
    // Set GPIB controls to device read mode
    setControls(DLAS);
    readWithEoi = true;  // In device mode we read with EOI by default
  }

#ifdef DEBUG_GPIBbus_RECEIVE
  DB_PRINT(F("Start listen ->"), "");
  DB_PRINT(F("Before loop flags:"), "");
  DB_PRINT(F("TRNb: "), txBreak);
  DB_PRINT(F("rEOI: "), readWithEoi);
//  DB_PRINT(F("ATN:  "), (isAsserted(ATN ? 1 : 0));
#endif

  // Ready the data bus
  readyGpibDbus();

  // Perform read of data (r=0: data read OK; r>0: GPIB read error);
  while (hstate == HANDSHAKE_COMPLETE) {

    // txBreak > 0 indicates break condition
    if (txBreak) {
      rstate = RECEIVE_BREAK;
      break;
    }

    // ATN asserted
    if (isAsserted(ATN_PIN)) {
      rstate = RECEIVE_ATN;
      break;
    }

    // Read the next character on the GPIB bus
    hstate = readByte(&bytes[0], readWithEoi, &eoiDetected);

    // If IFC or ATN asserted then break here
    if (hstate == IFC_ASSERTED) {
      rstate = RECEIVE_IFC;
      break;
    }

    if (hstate == ATN_ASSERTED) {
      rstate = RECEIVE_ATN;
      break;
    }

    // If successfully received character
    if (hstate == HANDSHAKE_COMPLETE) {
#ifdef DEBUG_GPIBbus_RECEIVE
      DB_HEX_PRINT(bytes[0]);
#else
      // Output the character to the serial port
      dataStream.print((char)bytes[0]);
#endif

      // Byte counter
      x++;

      // EOI detection enabled and EOI detected?
      if (readWithEoi) {
        if (eoiDetected) {
          rstate = RECEIVE_EOI;
          break;
        }
      } else {
        // Has a termination sequence been found ?
        if (detectEndByte) {
          if (bytes[0] == endByte) {
            rstate = RECEIVE_ENDCHAR;
            break;
          }
        } else {
          if (isTerminatorDetected(bytes, eor)) {
            rstate = RECEIVE_ENDL;
            break;
          }
        }
      }
      if ((maxSize > 0) && (x >= maxSize)) {
        rstate = RECEIVE_LIMIT;
        break;
      }      

      // Shift last three bytes in memory
      bytes[2] = bytes[1];
      bytes[1] = bytes[0];
    } else {
      // Stop (error or timeout)
      rstate = RECEIVE_ERR;
      break;
    }
  }

#ifdef DEBUG_GPIBbus_RECEIVE
  DB_RAW_PRINTLN();
  DB_PRINT(F("After loop flags:"), "");
  //  DB_PRINT(F("ATN: "), (isAsserted(ATN ? 1 : 0));
  DB_PRINT(F("TMO: "), cfg.rtmo);
  DB_PRINT(F("Bytes read:  "), x);
  DB_PRINT(F("<- End listen."), "");
#endif

  // Detected that EOI has been asserted
  if (eoiDetected) {
#ifdef DEBUG_GPIBbus_RECEIVE
    DB_PRINT(F("EOI detected!"), "");
#endif
    // If eot_enabled then add EOT character
    if (cfg.eot_en) dataStream.print(cfg.eot_ch);
  }

  // Verbose timeout error
#ifdef DEBUG_GPIBbus_RECEIVE
  if (state != HANDSHAKE_COMPLETE) {
    DB_PRINT(F("Timeout waiting for sender!"), "");
    DB_PRINT(F("Timeout waiting for transfer to complete!"), "");
  }
#endif

  // Return controller to idle state
  if (cfg.cmode == 2) {

    // Untalk bus and unlisten controller
/*
    if (unAddressDevice()) {
#ifdef DEBUG_GPIBbus_RECEIVE
      DB_PRINT(F("Failed to untalk bus"), "");
#endif
    }
*/
    // Set controller back to idle state
    setControls(CIDS);

  } else {
    // Set device back to idle state
    setControls(DIDS);
  }

  // Reset break flag
  if (txBreak) txBreak = false;

#ifdef DEBUG_GPIBbus_RECEIVE
  DB_PRINT(F("done."), "");
#endif
/*
  if (hstate == HANDSHAKE_COMPLETE) {
    return OK;
  } else {
    return ERR;
  }
*/
  return rstate;
}


/***** Send a series of characters as data to the GPIB bus *****/
void GPIBbus::sendData(const char *data, uint8_t dsize) {
  //  bool err = false;
  uint8_t tc;
  enum gpibHandshakeState state;

  switch (cfg.eos) {
    case 1:
    case 2:
      tc = 1;
      break;
    case 3:
      tc = 0;
      break;
    default:
      tc = 2;
  }
  // Set control pins for writing data (ATN unasserted)
  if (cfg.cmode == 2) {
    setControls(CTAS);
  } else {
    setControls(DTAS);
  }

#ifdef DEBUG_GPIBbus_SEND
  DB_PRINT(F("write data mode is set."), "");
  DB_PRINT(F("Begin send loop ->"), "");
#endif

  // Write the data string
  for (int i = 0; i < dsize; i++) {

    // If EOI asserting is on
    if (cfg.eoi) {
      // Send all characters
      if (tc) {
        state = writeByte(data[i], NO_EOI);  // Just send the character - EOI will be sent with the terminator
      } else {
        state = writeByte(data[i], (i == (dsize - 1)));  // Send EOI on last character
      }
    } else {
      // Otherwise ignore non-escaped CR, LF and ESC
      // Filter REMOVED as it afftects read of HP3478A cal dataf
      // if ((data[i] != CR) && (data[i] != LF) && (data[i] != ESC)) state = writeByte(data[i], NO_EOI);
      // Filter REMOVED as it affects read of HP3478A cal data
      // 
      state = writeByte(data[i], NO_EOI);
    }

#ifdef DEBUG_GPIBbus_SEND
    DB_RAW_PRINT(data[i]);
#endif

    if (state != HANDSHAKE_COMPLETE) break;
  }

#ifdef DEBUG_GPIBbus_SEND
  DB_PRINT(F("<- End of send loop."), "");
#endif

  // Terminators and EOI
  if ((state == HANDSHAKE_COMPLETE) && tc) {
    switch (cfg.eos) {
      case 1:
        writeByte(CR, cfg.eoi);
#ifdef DEBUG_GPIBbus_SEND
        DB_PRINT(F("appended CR"), (cfg.eoi ? " with EOI" : ""));
#endif
        break;
      case 2:
        writeByte(LF, cfg.eoi);
#ifdef DEBUG_GPIBbus_SEND
        DB_PRINT(F("appended LF"), (cfg.eoi ? " with EOI" : ""));
#endif
        break;
      case 3:
        break;
      default:
        writeByte(CR, NO_EOI);
        writeByte(LF, cfg.eoi);
#ifdef DEBUG_GPIBbus_SEND
        DB_PRINT(F("appended CRLF"), (cfg.eoi ? " with EOI" : ""));
#endif
    }
  }

  if (cfg.cmode == 2) {  // Controller mode
    // Controller - set lines to idle
    setControls(CIDS);
  } else {  // Device mode
    // Set control lines to idle
    setControls(DIDS);
  }

#ifdef DEBUG_GPIBbus_SEND
  DB_PRINT(F("done."), "");
#endif
}



/**************************************************/
/***** FUCTIONS TO READ/WRITE DATA TO STORAGE *****/
/***** vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv *****/

#ifdef EN_STORAGE


#endif

/***** ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ *****/
/***** FUCTIONS TO READ/WRITE DATA TO STORAGE *****/
/**************************************************/



/***** Signal to break a GPIB transmission *****/
void GPIBbus::signalBreak() {
  txBreak = true;
}


/***** Control the GPIB bus - set various GPIB states *****/
/*
 * state is a predefined state (CINI, CIDS, CCMS, CLAS, CTAS, DINI, DIDS, DLAS, DTAS);
 * Bits control lines as follows: 8-ATN, 7-SRQ, 6-REN, 5-EOI, 4-DAV, 3-NRFD, 2-NDAC, 1-IFC
 * setGpibState byte1 (databits) : State - 0=LOW, 1=HIGH/INPUT_PULLUP; Direction - 0=input, 1=output;
 * setGpibState byte2 (mask)     : 0=unaffected, 1=enabled
 * setGpibState byte3 (mode)     : 0=set pin state, 1=set pin direction
 */
void GPIBbus::setControls(uint8_t state) {

  // Switch state
  switch (state) {

    // Controller states
    case CINI:  // Initialisation
//    Serial.println(F("Set CINI OP_CTRL:"));
      setOperatingMode(OP_CTRL);
//      gpioFuncList();
//    Serial.println(F("Set CINI TM_IDLE:"));
      setTransmitMode(TM_IDLE);
//      gpioFuncList();
//    Serial.println(F("Assert REN_BIT:"));
      assertSignal(REN_BIT);
//    gpioFuncList();
#ifdef SN7516X
      digitalWrite(SN7516X_TE, LOW);
#ifdef SN7516X_DC
      digitalWrite(SN7516X_DC, LOW);
#endif
#ifdef SN7516X_SC
      digitalWrite(SN7516X_SC, HIGH);
#endif
#endif
#ifdef DEBUG_GPIBbus_CONTROL
      DB_PRINT(F("Initialised GPIB control mode"), "");
#endif
      break;


    case CIDS:  // Controller idle state
      clearSignal(ATN_BIT);
      setTransmitMode(TM_IDLE);
#ifdef SN7516X
      digitalWrite(SN7516X_TE, LOW);
#endif
#ifdef DEBUG_GPIBbus_CONTROL
      DB_PRINT(F("Set GPIB lines to idle state"), "");
#endif
      break;


    case CCMS:  // Controller active - send commands
      setTransmitMode(TM_SEND);
      assertSignal(ATN_BIT);
#ifdef SN7516X
      digitalWrite(SN7516X_TE, HIGH);
#endif
#ifdef DEBUG_GPIBbus_CONTROL
      DB_PRINT(F("Set GPIB lines for sending a command"), "");
#endif
      break;


    case CLAS:  // Controller - read data bus
      // Set state for receiving data
      clearSignal(ATN_BIT);
      setTransmitMode(TM_RECV);
#ifdef SN7516X
      digitalWrite(SN7516X_TE, LOW);
#endif
#ifdef DEBUG_GPIBbus_CONTROL
      DB_PRINT(F("Set GPIB lines for reading data"), "");
#endif
      break;


    case CTAS:  // Controller - write data bus
      clearSignal(ATN_BIT);
      setTransmitMode(TM_SEND);
#ifdef SN7516X
      digitalWrite(SN7516X_TE, HIGH);
#endif
#ifdef DEBUG_GPIBbus_CONTROL
      DB_PRINT(F("Set GPIB lines for writing data"), "");
#endif
      break;


    /* Bits control lines as follows: 8-ATN, 7-SRQ, 6-REN, 5-EOI, 4-DAV, 3-NRFD, 2-NDAC, 1-IFC */

    // Listener states
    case DINI:  // Listner initialisation


#ifdef SN7516X
      digitalWrite(SN7516X_TE, HIGH);
#ifdef SN7516X_DC
      digitalWrite(SN7516X_DC, HIGH);
#endif
#ifdef SN7516X_SC
      digitalWrite(SN7516X_SC, LOW);
#endif
#endif
      clearAllSignals();
      setOperatingMode(OP_DEVI);  // Set up for device mode
      // Set data bus to idle state
      readyGpibDbus();
#ifdef DEBUG_GPIBbus_CONTROL
      DB_PRINT(F("Initialised GPIB listener mode"), "");
#endif
      break;


    case DIDS:  // Device idle state
#ifdef SN7516X
      digitalWrite(SN7516X_TE, HIGH);
#endif
      setTransmitMode(TM_IDLE);
      // Set data bus to idle state
      readyGpibDbus();
#ifdef DEBUG_GPIBbus_CONTROL
      DB_PRINT(F("Set GPIB lines to idle state"), "");
#endif
      break;


    case DLAS:  // Device listner active (actively listening - can handshake)
#ifdef SN7516X
      digitalWrite(SN7516X_TE, LOW);
#endif
      setTransmitMode(TM_RECV);
#ifdef DEBUG_GPIBbus_CONTROL
      DB_PRINT(F("Set GPIB lines to idle state"), "");
#endif
      break;


    case DTAS:  // Device talker active (sending data)
#ifdef SN7516X
      digitalWrite(SN7516X_TE, HIGH);
#endif
      setTransmitMode(TM_SEND);
#ifdef DEBUG_GPIBbus_CONTROL
      DB_PRINT(F("Set GPIB lines for listening as addresed device"), "");
#endif
      break;
#ifdef DEBUG_GPIBbus_CONTROL
    default:
      // Should never get here!
      DB_PRINT(F("Unknown GPIB state requested!"), "");
#endif
  }

  // Save state
  cstate = state;
}


/***** Set GPIP control state using numeric input (xdiag_h) *****/
void GPIBbus::setControlVal(uint8_t value) {
  setGpibCtrlDir(0xFF, 0xFF); // Set all as outputs
  setGpibCtrlState(value, 0xFF);
}


/***** Set GPIB data bus to specific value (xdiag_h) *****/
void GPIBbus::setDataVal(uint8_t value) {
  setGpibDbus(value);
}


/***** Unaddress device *****/
bool GPIBbus::unAddressDevice() {
  // De-bounce
  delayMicroseconds(30);
  // Utalk/unlisten
  if (sendCmd(GC_UNL)) return ERR;
  if (sendCmd(GC_UNT)) return ERR;
  // Clear secondary address
//  cfg.saddr = 0xFF;
  // Clear flag
  deviceAddressed = TONONE;
#ifdef DEBUG_GPIBbus_DEVICE
  DB_PRINT(F("done."), "");
#endif
  return OK;
}


/***** Untalk bus then address a device *****/
bool GPIBbus::addressDevice(uint8_t pri, uint8_t sec=0xFF, uint8_t dir=TOLISTEN) {

  if (pri>30) return ERR;

  if ( sec<0x60 || (sec>0x7E && sec!=0xFF) ) return ERR;

  if (sendCmd(GC_UNL)) return ERR;
  if (sendCmd(GC_UNT)) return ERR;

//Serial.println(F("Addressing..."));
#ifdef DEBUG_GPIBbus_DEVICE
  DB_PRINT(F("addressDevice: pri="), pri);
  DB_PRINT(F("addressDevice: sec="), sec);
#endif

  if (dir == TOTALK) {
    // Device to talk, controller to listen
    if (sendCmd(GC_TAD + pri)) return ERR;
    // Secondary address?
    if (sec != 0xFF) {
      if (sendCmd(sec)) return ERR;
    }
    deviceAddressed = TOTALK;
  } else {
    // Device to listen, controller to talk
    if (sendCmd(GC_LAD + pri)) return ERR;
    // Secondary address?
    if (sec != 0xFF) {
      if (sendCmd(sec)) return ERR;
    }
    deviceAddressed = TOLISTEN;
  }

  // Set flag
//  deviceAddressed = true;
  return OK;
}


/***** Return status device addressing (Controller mode) *****/
/*
 * true = device has been addressed; false = device has not been addressed
 */
uint8_t GPIBbus::haveAddressedDevice() {
  return deviceAddressed;
}


/***** Device is addressed to listen? (Device mode) *****/
bool GPIBbus::isDeviceAddressedToListen() {
  if (cstate == DLAS) return true;
  return false;
}


/***** Device is addressed to talk? (Device mode) *****/
bool GPIBbus::isDeviceAddressedToTalk() {
  if (cstate == DTAS) return true;
  return false;
}


/***** Device is not addressed? (Device mode) *****/
bool GPIBbus::isDeviceInIdleState() {
  if (cstate == DIDS) return true;
  return false;
}


/***** Clear the data bus and set to listen state *****/
void GPIBbus::clearDataBus() {
  readyGpibDbus();
}


/***** Read a SINGLE BYTE of data from the GPIB bus using 3-way handshake *****/
/*
 * (- this function is called in a loop to read data    )
 * (- the GPIB bus must already be configured to listen )
 */
enum gpibHandshakeState GPIBbus::readByte(uint8_t *db, bool readWithEoi, bool *eoi) {

  unsigned long startMillis = millis();
  unsigned long currentMillis = startMillis + 1;
  const unsigned long timeval = cfg.rtmo;
  enum gpibHandshakeState gpibState = HANDSHAKE_START;

  bool atnStat = isAsserted(ATN_PIN);  // Capture state of ATN
  *eoi = false;

  // Wait for interval to expire
  while ((unsigned long)(currentMillis - startMillis) < timeval) {

    if (cfg.cmode == 1) {
      // If IFC has been asserted then abort
      if (isAsserted(IFC_PIN)) {
#ifdef DEBUG_GPIBbus_RECEIVE
        DB_PRINT(F("IFC detected]"), "");
#endif
        gpibState = IFC_ASSERTED;
        break;
      }

      // ATN unasserted during handshake - not ready yet so abort (and exit ATN loop)
      if (atnStat && !isAsserted(ATN_PIN)) {
        gpibState = ATN_ASSERTED;
        break;
      }
    }

    if (gpibState == HANDSHAKE_START) {
      // Unassert NRFD (we are ready for more data)
      clearSignal(NRFD_BIT);
      gpibState = WAIT_FOR_DATA;
    }

    if (gpibState == WAIT_FOR_DATA) {
      // Wait for DAV to go LOW indicating talker has finished setting data lines..
      if (getGpibPinState(DAV_PIN) == LOW) {
        // Assert NRFD (Busy reading data)
        assertSignal(NRFD_BIT);
        gpibState = READ_DATA;
      }
    }

    if (gpibState == READ_DATA) {
      // Check for EOI signal
      if (readWithEoi && isAsserted(EOI_PIN)) *eoi = true;
      // read from DIO
      *db = readGpibDbus();
      // Unassert NDAC signalling data accepted
      clearSignal(NDAC_BIT);
      gpibState = DATA_ACCEPTED;
    }

    if (gpibState == DATA_ACCEPTED) {
      // Wait for DAV to go HIGH indicating data no longer valid (i.e. transfer complete)
      if (getGpibPinState(DAV_PIN) == HIGH) {
        // Re-assert NDAC - handshake complete, ready to accept data again
        assertSignal(NDAC_BIT);
        gpibState = HANDSHAKE_COMPLETE;
        break;
      }
    }

    // Increment time
    currentMillis = millis();
  }

  // Otherwise return stage
#ifdef DEBUG_GPIBbus_RECEIVE
  if ((gpibState == HANDSHAKE_START) || (gpibState == DATA_ACCEPTED)) {
    DB_PRINT(F("DAV timout!"), "");
  } else {
    DB_PRINT(F("Handshake error!"), "");
  }
#endif

  return gpibState;
}


enum gpibHandshakeState GPIBbus::writeByte(uint8_t db, bool isLastByte) {
  unsigned long startMillis = millis();
  unsigned long currentMillis = startMillis + 1;
  const unsigned long timeval = cfg.rtmo;
  enum gpibHandshakeState gpibState = HANDSHAKE_START;

  // Wait for interval to expire
  while ((unsigned long)(currentMillis - startMillis) < timeval) {

    if (cfg.cmode == 1) {
      // If IFC has been asserted then abort
      if (isAsserted(IFC_PIN)) {
        setControls(DLAS);
#ifdef DEBUG_GPIBbus_SEND
        DB_PRINT(F("IFC detected!"), "");
#endif
        gpibState = IFC_ASSERTED;
        break;
      }

      // If ATN has been asserted we need to abort and listen
      if (isAsserted(ATN_PIN)) {
        setControls(DLAS);
#ifdef DEBUG_GPIBbus_SEND
        DB_PRINT(F("ATN detected!"), "");
#endif
        gpibState = ATN_ASSERTED;
        break;
      }
    }

    // Wait for NDAC to go LOW (indicating that devices (stage==4) || (stage==8) ) are at attention)
    if (gpibState == HANDSHAKE_START) {
      if (getGpibPinState(NDAC_PIN) == LOW) gpibState = WAIT_FOR_RECEIVER_READY;
    }

    // Wait for NRFD to go HIGH (indicating that receiver is ready)
    if (gpibState == WAIT_FOR_RECEIVER_READY) {
      if (getGpibPinState(NRFD_PIN) == HIGH) gpibState = PLACE_DATA;
    }

    if (gpibState == PLACE_DATA) {
      // Place data on the bus
      setGpibDbus(db);
      if (cfg.eoi && isLastByte) {
        // If EOI enabled and this is the last byte then assert DAV and EOI
#ifdef DEBUG_GPIBbus_SEND
        DB_PRINT(F("Asserting EOI..."), "");
#endif
        assertSignal(DAV_BIT | EOI_BIT);
      } else {
        // Assert DAV (data is valid - ready to collect)
        assertSignal(DAV_BIT);
      }
      gpibState = DATA_READY;
    }

    if (gpibState == DATA_READY) {
      // Wait for NRFD to go LOW (receiver accepting data)
      if (getGpibPinState(NRFD_PIN) == LOW) gpibState = RECEIVER_ACCEPTING;
    }

    if (gpibState == RECEIVER_ACCEPTING) {
      // Wait for NDAC to go HIGH (data accepted)
      if (getGpibPinState(NDAC_PIN) == HIGH) {
        gpibState = HANDSHAKE_COMPLETE;
        break;
      }
    }

    // Increment time
    currentMillis = millis();
  }

  // Handshake complete
  if (gpibState == HANDSHAKE_COMPLETE) {
    if (cfg.eoi && isLastByte) {
      // If EOI enabled and this is the last byte then un-assert both DAV and EOI
      clearSignal(DAV_BIT | EOI_BIT);
    } else {
      // Unassert DAV
      clearSignal(DAV_BIT);
    }
    // Reset the data bus
    setGpibDbus(0);
    return gpibState;
  }

  // Otherwise timeout or ATN/IFC return stage at which it ocurred
#ifdef DEBUG_GPIBbus_SEND
  switch (gpibState) {
    case HANDSHAKE_START:
      DB_PRINT(F("NDAC LO timeout!"), "");
      break;
    case WAIT_FOR_RECEIVER_READY:
      DB_PRINT(F("NRFD HI timout!"), "");
      break;
    case DATA_READY:
      DB_PRINT(F("NRFD LO timout!"), "");
      break;
    case RECEIVER_ACCEPTING:
      DB_PRINT(F("NDAC HI timout!"), "");
      break;
    default:
      DB_PRINT(F("Handshake error!"), "");
  }
#endif

  return gpibState;
}


/***** ^^^^^^^^^^^^^^^^^^^^^^^^^^^ *****/
/***** GPIB CLASS PUBLIC FUNCTIONS *****/
/***************************************/




/***************************************/
/***** GPIB CLASS PRIVATE FUNCTIONS *****/
/***** ^^^^^^^^^^^^^^^^^^^^^^^^^^^ *****/



/********** PRIVATE FUNCTIONS **********/


/***** Check for terminator *****/
bool GPIBbus::isTerminatorDetected(uint8_t bytes[3], uint8_t eorSequence) {
  // Look for specified terminator (CR+LF by default)
  switch (eorSequence) {
    case 0:
      // CR+LF terminator
      if (bytes[0] == LF && bytes[1] == CR) return true;
      break;
    case 1:
      // CR only as terminator
      if (bytes[0] == CR) return true;
      break;
    case 2:
      // LF only as terminator
      if (bytes[0] == LF) return true;
      break;
    case 3:
      // No terminator (will rely on timeout)
      break;
    case 4:
      // Keithley can use LF+CR instead of CR+LF
      if (bytes[0] == CR && bytes[1] == LF) return true;
      break;
    case 5:
      // Solarton (possibly others) can also use ETX (0x03)
      if (bytes[0] == 0x03) return true;
      break;
    case 6:
      // Solarton (possibly others) can also use CR+LF+ETX (0x03)
      if (bytes[0] == 0x03 && bytes[1] == LF && bytes[2] == CR) return true;
      break;
    default:
      // Use CR+LF terminator by default
      if (bytes[0] == LF && bytes[1] == CR) return true;
      break;
  }
  return false;
}


/***** ^^^^^^^^^^^^^^^^^^^^^^^^^^^^ *****/
/***** GPIB CLASS PRIVATE FUNCTIONS *****/
/****************************************/
