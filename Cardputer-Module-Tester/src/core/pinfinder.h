#pragma once
// "Find chip pins": tries every spare GPIO as a chip select on the given SPI
// bus and reports which pin a CC1101 and which pin an NRF24L01 answers on.
// It also reads the CC1101 on `ccCs` twice (SPI hardware and bit-banged) so
// a bus problem can be told apart from a wiring problem. `ccCs` may be -1.
// `skipPin` (the CC1101 GDO0 pin, an output) is not driven during the scan.
namespace pinfinder {

void run(int sck, int miso, int mosi, int ccCs, int skipPin = -1);

}  // namespace pinfinder
