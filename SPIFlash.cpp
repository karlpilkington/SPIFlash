/*
 * Copyright (c) 2013 by Felix Rusu <felix@lowpowerlab.com>
 * SPI Flash memory library for arduino/moteino.
 * This works with 256byte/page SPI flash memory
 * For instance a 4MBit (512Kbyte) flash chip will have 2048 pages: 256*2048 = 524288 bytes (512Kbytes)
 * Minimal modifications should allow chips that have different page size but modifications
 * DEPENDS ON: Arduino SPI library
 *
 * Updated Jan. 5, 2015, TomWS1, modified writeBytes to allow blocks > 256 bytes and handle page misalignment.
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of either the GNU General Public License version 2
 * or the GNU Lesser General Public License version 2.1, both as
 * published by the Free Software Foundation.
 */

#include <SPIFlash.h>

byte SPIFlash::UNIQUEID[8];

/// IMPORTANT: NAND FLASH memory requires erase before write, because
///            it can only transition from 1s to 0s and only the erase command can reset all 0s to 1s
/// See http://en.wikipedia.org/wiki/Flash_memory
/// The smallest range that can be erased is a sector (4K, 32K, 64K); there is also a chip erase command

/// Constructor. JedecID is optional but recommended, since this will ensure that the device is present and has a valid response
/// get this from the datasheet of your flash chip
/// Example for Atmel-Adesto 4Mbit AT25DF041A: 0x1F44 (page 27: http://www.adestotech.com/sites/default/files/datasheets/doc3668.pdf)
/// Example for Winbond 4Mbit W25X40CL: 0xEF30 (page 14: http://www.winbond.com/NR/rdonlyres/6E25084C-0BFE-4B25-903D-AE10221A0929/0/W25X40CL.pdf)
SPIFlash::SPIFlash(uint8_t slaveSelectPin, uint16_t jedecID) {
  _slaveSelectPin = slaveSelectPin;
  _jedecID = jedecID;
}

/// Select the flash chip
void SPIFlash::select() {
  noInterrupts();
  //save current SPI settings
  _SPCR = SPCR;
  _SPSR = SPSR;
  //set FLASH chip SPI settings
  SPI.setDataMode(SPI_MODE0);
  SPI.setBitOrder(MSBFIRST);
  SPI.setClockDivider(SPI_CLOCK_DIV4); //decided to slow down from DIV2 after SPI stalling in some instances, especially visible on mega1284p when RFM69 and FLASH chip both present
  SPI.begin();
  digitalWrite(_slaveSelectPin, LOW);
}

/// UNselect the flash chip
void SPIFlash::unselect() {
  digitalWrite(_slaveSelectPin, HIGH);
  //restore SPI settings to what they were before talking to the FLASH chip
  SPCR = _SPCR;
  SPSR = _SPSR;
  interrupts();
}

/// setup SPI, read device ID etc...
boolean SPIFlash::initialize()
{
  _SPCR = SPCR;
  _SPSR = SPSR;
  pinMode(_slaveSelectPin, OUTPUT);
  unselect();
  wakeup();
  
  if (_jedecID == 0 || readDeviceId() == _jedecID) {
    command(SPIFLASH_STATUSWRITE, true); // Write Status Register
    SPI.transfer(0);                     // Global Unprotect
    unselect();
    return true;
  }
  return false;
}

/// Get the manufacturer and device ID bytes (as a short word)
word SPIFlash::readDeviceId()
{
#if defined(__AVR_ATmega32U4__) // Arduino Leonardo, MoteinoLeo
  command(SPIFLASH_IDREAD); // Read JEDEC ID
#else
  select();
  SPI.transfer(SPIFLASH_IDREAD);
#endif
  word jedecid = SPI.transfer(0) << 8;
  jedecid |= SPI.transfer(0);
  unselect();
  return jedecid;
}

/// Get the 64 bit unique identifier, stores it in UNIQUEID[8]. Only needs to be called once, ie after initialize
/// Returns the byte pointer to the UNIQUEID byte array
/// Read UNIQUEID like this:
/// flash.readUniqueId(); for (byte i=0;i<8;i++) { Serial.print(flash.UNIQUEID[i], HEX); Serial.print(' '); }
/// or like this:
/// flash.readUniqueId(); byte* MAC = flash.readUniqueId(); for (byte i=0;i<8;i++) { Serial.print(MAC[i], HEX); Serial.print(' '); }
byte* SPIFlash::readUniqueId()
{
  command(SPIFLASH_MACREAD);
  SPI.transfer(0);
  SPI.transfer(0);
  SPI.transfer(0);
  SPI.transfer(0);
  for (byte i=0;i<8;i++)
    UNIQUEID[i] = SPI.transfer(0);
  unselect();
  return UNIQUEID;
}

/// read 1 byte from flash memory
byte SPIFlash::readByte(long addr) {
  command(SPIFLASH_ARRAYREADLOWFREQ);
  SPI.transfer(addr >> 16);
  SPI.transfer(addr >> 8);
  SPI.transfer(addr);
  byte result = SPI.transfer(0);
  unselect();
  return result;
}

/// read unlimited # of bytes
void SPIFlash::readBytes(long addr, void* buf, word len) {
  command(SPIFLASH_ARRAYREAD);
  SPI.transfer(addr >> 16);
  SPI.transfer(addr >> 8);
  SPI.transfer(addr);
  SPI.transfer(0); //"dont care"
  for (word i = 0; i < len; ++i)
    ((byte*) buf)[i] = SPI.transfer(0);
  unselect();
}

/// Send a command to the flash chip, pass TRUE for isWrite when its a write command
void SPIFlash::command(byte cmd, boolean isWrite){
#if defined(__AVR_ATmega32U4__) // Arduino Leonardo, MoteinoLeo
  DDRB |= B00000001;            // Make sure the SS pin (PB0 - used by RFM12B on MoteinoLeo R1) is set as output HIGH!
  PORTB |= B00000001;
#endif
  if (isWrite)
  {
    command(SPIFLASH_WRITEENABLE); // Write Enable
    unselect();
  }
  //wait for any write/erase to complete
  //  a time limit cannot really be added here without it being a very large safe limit
  //  that is because some chips can take several seconds to carry out a chip erase or other similar multi block or entire-chip operations
  //  a recommended alternative to such situations where chip can be or not be present is to add a 10k or similar weak pulldown on the
  //  open drain MISO input which can read noise/static and hence return a non 0 status byte, causing the while() to hang when a flash chip is not present
  while(busy());
  select();
  SPI.transfer(cmd);
}

/// check if the chip is busy erasing/writing
boolean SPIFlash::busy()
{
  /*
  select();
  SPI.transfer(SPIFLASH_STATUSREAD);
  byte status = SPI.transfer(0);
  unselect();
  return status & 1;
  */
  return readStatus() & 1;
}

/// return the STATUS register
byte SPIFlash::readStatus()
{
  select();
  SPI.transfer(SPIFLASH_STATUSREAD);
  byte status = SPI.transfer(0);
  unselect();
  return status;
}


/// Write 1 byte to flash memory
/// WARNING: you can only write to previously erased memory locations (see datasheet)
///          use the block erase commands to first clear memory (write 0xFFs)
void SPIFlash::writeByte(long addr, uint8_t byt) {
  command(SPIFLASH_BYTEPAGEPROGRAM, true);  // Byte/Page Program
  SPI.transfer(addr >> 16);
  SPI.transfer(addr >> 8);
  SPI.transfer(addr);
  SPI.transfer(byt);
  unselect();
}

/// write multiple bytes to flash memory (up to 64K)
/// WARNING: you can only write to previously erased memory locations (see datasheet)
///          use the block erase commands to first clear memory (write 0xFFs)
/// This version handles both page alignment and data blocks larger than 256 bytes.
///
void SPIFlash::writeBytes(long addr, const void* buf, uint16_t len) {
  uint16_t n;
  uint16_t maxBytes = 256-(addr%256);  // force the first set of bytes to stay within the first page
  uint16_t offset = 0;
  while (len>0)
  {
    n = (len<=maxBytes) ? len : maxBytes;
    command(SPIFLASH_BYTEPAGEPROGRAM, true);  // Byte/Page Program
    SPI.transfer(addr >> 16);
    SPI.transfer(addr >> 8);
    SPI.transfer(addr);
    
    for (uint16_t i = 0; i < n; i++)
      SPI.transfer(((byte*) buf)[offset + i]);
    unselect();
    
    addr+=n;  // adjust the addresses and remaining bytes by what we've just transferred.
    offset +=n;
    len -= n;
    maxBytes = 256;   // now we can do up to 256 bytes per loop
  }
}

/// erase entire flash memory array
/// may take several seconds depending on size, but is non blocking
/// so you may wait for this to complete using busy() or continue doing
/// other things and later check if the chip is done with busy()
/// note that any command will first wait for chip to become available using busy()
/// so no need to do that twice
void SPIFlash::chipErase() {
  command(SPIFLASH_CHIPERASE, true);
  unselect();
}

/// erase a 4Kbyte block
void SPIFlash::blockErase4K(long addr) {
  command(SPIFLASH_BLOCKERASE_4K, true); // Block Erase
  SPI.transfer(addr >> 16);
  SPI.transfer(addr >> 8);
  SPI.transfer(addr);
  unselect();
}

/// erase a 32Kbyte block
void SPIFlash::blockErase32K(long addr) {
  command(SPIFLASH_BLOCKERASE_32K, true); // Block Erase
  SPI.transfer(addr >> 16);
  SPI.transfer(addr >> 8);
  SPI.transfer(addr);
  unselect();
}

void SPIFlash::sleep() {
  command(SPIFLASH_SLEEP);
  unselect();
}

void SPIFlash::wakeup() {
  command(SPIFLASH_WAKE);
  unselect();
}

/// cleanup
void SPIFlash::end() {
  SPI.end();
}