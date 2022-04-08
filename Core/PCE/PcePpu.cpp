#include "stdafx.h"
#include "PCE/PcePpu.h"
#include "PCE/PceMemoryManager.h"
#include "PCE/PceControlManager.h"
#include "PCE/PceConsole.h"
#include "Shared/Video/VideoDecoder.h"
#include "Shared/NotificationManager.h"
#include "EventType.h"

PcePpu::PcePpu(Emulator* emu, PceConsole* console)
{
	_emu = emu;
	_console = console;
	_vram = new uint16_t[0x8000];
	_spriteRam = new uint16_t[0x100];
	_paletteRam = new uint16_t[0x200];
	_outBuffer = new uint16_t[256 * 242];

	memset(_vram, 0, 0x10000);
	memset(_paletteRam, 0, 0x400);

	_emu->RegisterMemory(MemoryType::PceVideoRam, _vram, 0x8000 * sizeof(uint16_t));
	_emu->RegisterMemory(MemoryType::PcePaletteRam, _paletteRam, 0x200 * sizeof(uint16_t));
	_emu->RegisterMemory(MemoryType::PceSpriteRam, _spriteRam, 0x100 * sizeof(uint16_t));
}

PcePpu::~PcePpu()
{
	delete[] _vram;
	delete[] _paletteRam;
	delete[] _spriteRam;
	delete[] _outBuffer;
}

PcePpuState& PcePpu::GetState()
{
	return _state;
}

void PcePpu::Exec()
{
	_state.Cycle++;
	if(_state.Cycle == 341) {
		_state.Cycle = 0;

		uint16_t topBorder = _state.VertDisplayStart + _state.VertSyncWidth;
		uint16_t screenEnd = topBorder + _state.VertDisplayWidth;
		bool drawOverscan = true;
		if(_state.DisplayCounter >= topBorder) {
			if(_state.DisplayCounter < screenEnd) {
				if(_state.Scanline >= 14) {
					drawOverscan = false;
				}
			} else {
				if(_state.DisplayCounter == screenEnd) {
					//End of display, trigger irq?
					//Update flags that were locked during burst mode
					_state.BackgroundEnabled = _state.NextBackgroundEnabled;
					_state.SpritesEnabled = _state.NextSpritesEnabled;

					if(_state.SatbTransferPending || _state.RepeatSatbTransfer) {
						_state.SatbTransferPending = false;
						_state.SatbTransferRunning = true;

						//Sprite transfer ends 4 lines after vertical blank start?
						_state.SatbTransferCycleCounter = 341*4;
					}

					if(_state.EnableVerticalBlankIrq) {
						_state.VerticalBlank = true;
						_console->GetMemoryManager()->SetIrqSource(PceIrqSource::Irq1);
					}
				}
			}
		}

		if(_state.SatbTransferRunning) {
			_state.SatbTransferCycleCounter--;
			if(_state.SatbTransferCycleCounter) {
				for(int i = 0; i < 256; i++) {
					uint16_t value = _vram[(_state.SatbBlockSrc + i) & 0x7FFF];
					_emu->ProcessPpuWrite<CpuType::Pce>(i << 1, value & 0xFF, MemoryType::PceSpriteRam);
					_emu->ProcessPpuWrite<CpuType::Pce>((i << 1) + 1, value >> 8, MemoryType::PceSpriteRam);
					_spriteRam[i] = value;
				}

				_state.SatbTransferRunning = false;

				if(_state.VramSatbIrqEnabled) {
					_state.SatbTransferDone = true;
					_console->GetMemoryManager()->SetIrqSource(PceIrqSource::Irq1);
				}
			}
		}

		DrawScanline(drawOverscan);

		_state.Scanline++;
		_state.DisplayCounter++;

		if(_state.DisplayCounter >= screenEnd + _state.VertEndPosVcr + 3 && _state.Scanline < 14+242) {
			//re-start displaying picture
			_state.DisplayCounter = 0;
		}

		if(_state.EnableScanlineIrq) {
			if(_state.Scanline < topBorder) {
				int scanlineValue = (int)_state.Scanline - topBorder + 263;
				if(scanlineValue == _state.RasterCompareRegister) {
					_state.ScanlineDetected = true;
				}
			} else {
				if(_state.Scanline - topBorder == _state.RasterCompareRegister - 0x40) {
					_state.ScanlineDetected = true;
				}
			}

			if(_state.ScanlineDetected) {
				_console->GetMemoryManager()->SetIrqSource(PceIrqSource::Irq1);
			}
		}

		if(_state.Scanline == 256) {
			_state.FrameCount++;
			SendFrame();
		} else if(_state.Scanline == 263) {
			_state.Scanline = 0;
			_state.DisplayCounter = 0;
			_state.BurstModeEnabled = !_state.BackgroundEnabled && !_state.SpritesEnabled;
			_state.BgScrollYLatch = _state.BgScrollY;
		}
	}

	_emu->ProcessPpuCycle<CpuType::Pce>();
}

template<uint8_t bpp>
uint8_t GetTilePixelColor(const uint16_t* chrData, const uint8_t shift)
{
	uint8_t color;
	if(bpp == 2) {
		//TODO unused
		color = (chrData[0] >> shift) & 0x01;
		color |= (chrData[0] >> (7 + shift)) & 0x02;
	} else if(bpp == 4) {
		color = (chrData[0] >> shift) & 0x01;
		color |= (chrData[0] >> (7 + shift)) & 0x02;
		color |= ((chrData[8] >> shift) & 0x01) << 2;
		color |= ((chrData[8] >> (7 + shift)) & 0x02) << 2;
	} else if(bpp == 5) {
		//TODO hack, fix
		color = (chrData[0] >> shift) & 0x01;
		color |= ((chrData[16] >> shift) & 0x01) << 1;
		color |= ((chrData[32] >> shift) & 0x01) << 2;
		color |= ((chrData[48] >> shift) & 0x01) << 3;
	} else {
		throw std::runtime_error("unsupported bpp");
	}
	return color;
}

void PcePpu::DrawScanline(bool drawOverscan)
{
	if(_state.Scanline < 14 || _state.Scanline >= 256) {
		//Only 242 rows can be shown
		return;
	}

	uint16_t topBorder = _state.VertDisplayStart + _state.VertSyncWidth;
	uint16_t row = _state.DisplayCounter - topBorder;
	uint16_t outputOffset = ((int)_state.Scanline - 14) * 256;

	if(_state.BurstModeEnabled || drawOverscan) {
		//Draw sprite color 0 as background when display is disabled
		for(int column = 0; column < 256; column++) {
			_outBuffer[outputOffset + column] = _paletteRam[16 * 16];
		}
		return;
	}

	bool hasBg[256] = {};
	if(_state.BackgroundEnabled) {
		uint16_t screenY = (_state.BgScrollYLatch) & ((_state.RowCount * 8) - 1);
		for(int column = 0; column < 256; column++) {
			uint16_t screenX = (column + _state.BgScrollX) & ((_state.ColumnCount * 8) - 1);

			uint16_t batEntry = _vram[(screenY >> 3) * _state.ColumnCount + (screenX >> 3)];
			uint8_t palette = batEntry >> 12;
			uint16_t tileIndex = (batEntry & 0xFFF);

			uint16_t tileAddr = tileIndex * 16;
			uint8_t color = GetTilePixelColor<4>(_vram + ((tileAddr + (screenY & 0x07)) & 0x7FFF), 7 - (screenX & 0x07));
			if(color != 0) {
				hasBg[column] = true;
				_outBuffer[outputOffset + column] = _paletteRam[palette * 16 + color];
			} else {
				_outBuffer[outputOffset + column] = _paletteRam[0];
			}
		}
	} else {
		//Draw background color
		for(int column = 0; column < 256; column++) {
			_outBuffer[outputOffset + column] = _paletteRam[0];
		}
	}

	_state.BgScrollYLatch++;

	if(_state.SpritesEnabled) {
		bool hasSprite[256] = {};
		bool hasSprite0[256] = {};

		for(int i = 0; i < 64; i++) {
			int16_t y = (int16_t)(_spriteRam[i * 4] & 0x3FF) - 64;
			if(row < y) {
				//Sprite not visible on this line
				continue;
			}

			uint8_t height;
			uint16_t flags = _spriteRam[i * 4 + 3];
			switch((flags >> 12) & 0x03) {
				case 0: height = 16; break;
				case 1: height = 32; break;
				case 2: case 3: height = 64; break;
			}

			if(row >= y + height) {
				//Sprite not visible on this line
				continue;
			}

			uint16_t tileIndex = (_spriteRam[i * 4 + 2] & 0x7FF) >> 1;
			uint8_t width = (flags & 0x100) ? 32 : 16;
			int16_t spriteX = (int16_t)(_spriteRam[i * 4 + 1] & 0x3FF) - 32;

			if(spriteX + width <= 0 || spriteX >= 256) {
				//Sprite off-screen
				continue;
			}

			uint16_t spriteRow = row - y;
			if(width == 32) {
				tileIndex &= ~0x01;
			}
			if(height == 32) {
				tileIndex &= ~0x02;
			} else if(height == 64) {
				tileIndex &= ~0x06;
			}

			bool verticalMirror = (flags & 0x8000) != 0;
			bool horizontalMirror = (flags & 0x800) != 0;
			bool priority = (flags & 0x80) != 0;
			int yOffset = 0;
			int rowOffset = 0;
			if(verticalMirror) {
				yOffset = (height - spriteRow - 1) & 0x0F;
				rowOffset = (height - spriteRow - 1) >> 4;
			} else {
				yOffset = spriteRow & 0x0F;
				rowOffset = spriteRow >> 4;
			}

			uint16_t spriteTileY = tileIndex | (rowOffset << 1);

			for(int x = 0; x < width; x++) {
				if(spriteX + x < 0) {
					continue;
				} else if(spriteX + x >= 256) {
					//Offscreen
					break;
				}

				uint8_t xOffset;
				int columnOffset;
				if(horizontalMirror) {
					xOffset = (width - x - 1) & 0x0F;
					columnOffset = (width - x - 1) >> 4;
				} else {
					xOffset = x & 0x0F;
					columnOffset = x >> 4;
				}

				uint16_t spriteTile = spriteTileY | columnOffset;
				uint16_t tileStart = spriteTile * 64;

				uint16_t pixelStart = tileStart + yOffset;
				uint8_t shift = 15 - xOffset;

				uint8_t color = GetTilePixelColor<5>(_vram + (pixelStart & 0x7FFF), shift);

				if(color != 0) {
					if(hasSprite[spriteX + x]) {
						if(hasSprite0[spriteX + x] && _state.EnableCollisionIrq) {
							_state.Sprite0Hit = true;
							_console->GetMemoryManager()->SetIrqSource(PceIrqSource::Irq1);
						}
					} else {
						if(priority || hasBg[spriteX + x] == 0) {
							_outBuffer[outputOffset + spriteX + x] = _paletteRam[color + (flags & 0x0F) * 16 + 16 * 16];
						}
						
						hasSprite[spriteX + x] = true;
						if(i == 0) {
							hasSprite0[spriteX + x] = true;
						}
					}
				}
			}
		}
	}
}

void PcePpu::SendFrame()
{
	_emu->ProcessEvent(EventType::EndFrame);
	_emu->GetNotificationManager()->SendNotification(ConsoleNotificationType::PpuFrameDone, _outBuffer);

	RenderedFrame frame(_outBuffer, 256, 242, 1.0, _state.FrameCount, _console->GetControlManager()->GetPortStates());
	_emu->GetVideoDecoder()->UpdateFrame(frame, true, false);

	_emu->ProcessEndOfFrame();

	_console->GetControlManager()->UpdateInputState();
}

void PcePpu::LoadReadBuffer()
{
	//TODO timing - this needs to be done in-between rendering reads (based on mode, etc.)
	_state.ReadBuffer = _vram[_state.MemAddrRead & 0x7FFF];
	_emu->ProcessPpuRead<CpuType::Pce>((_state.MemAddrRead << 1), (uint8_t)_state.ReadBuffer, MemoryType::PceVideoRam);
	_emu->ProcessPpuRead<CpuType::Pce>((_state.MemAddrRead << 1) + 1, (uint8_t)(_state.ReadBuffer >> 8), MemoryType::PceVideoRam);
}

uint8_t PcePpu::ReadVdc(uint16_t addr)
{
	switch(addr & 0x03) {
		default:
		case 0: {
			//TODO BSY
			uint8_t result = 0;
			result |= _state.VerticalBlank ? 0x20 : 0x00;
			result |= _state.VramTransferDone ? 0x10 : 0x00;
			result |= _state.SatbTransferDone ? 0x08 : 0x00;
			result |= _state.ScanlineDetected ? 0x04 : 0x00;
			result |= _state.SpriteOverflow ? 0x02 : 0x00;
			result |= _state.Sprite0Hit ? 0x01 : 0x00;

			_state.VerticalBlank = false;
			_state.VramTransferDone = false;
			_state.SatbTransferDone = false;
			_state.ScanlineDetected = false;
			_state.SpriteOverflow = false;
			_state.Sprite0Hit = false;

			_console->GetMemoryManager()->ClearIrqSource(PceIrqSource::Irq1);
			return result;
		}

		case 1: return 0; //Unused, reads return 0

		//Reads to 2/3 will always return the read buffer, but the
		//read address will only increment when register 2 is selected
		case 2: return (uint8_t)_state.ReadBuffer;

		case 3:
			uint8_t value = _state.ReadBuffer >> 8;
			if(_state.CurrentReg == 0x02) {
				_state.MemAddrRead += _state.VramAddrIncrement;
				LoadReadBuffer();
			}
			return value;
	}
}

void PcePpu::WriteVdc(uint16_t addr, uint8_t value)
{
	switch(addr & 0x03) {
		case 0: _state.CurrentReg = value & 0x1F; break;

		case 1: break; //Unused, writes do nothing

		case 2:
		case 3:
			bool msb = (addr & 0x03) == 0x03;

			switch(_state.CurrentReg) {
				case 0x00: UpdateReg(_state.MemAddrWrite, value, msb); break;

				case 0x01:
					UpdateReg(_state.MemAddrRead, value, msb);
					if(msb) {
						LoadReadBuffer();
					}
					break;

				case 0x02:
					UpdateReg(_state.VramData, value, msb);
					if(msb) {
						if(_state.MemAddrWrite < 0x8000) {
							//Ignore writes to mirror at $8000+
							//TODO timing
							_emu->ProcessPpuWrite<CpuType::Pce>(_state.MemAddrWrite << 1, _state.VramData & 0xFF, MemoryType::PceVideoRam);
							_emu->ProcessPpuWrite<CpuType::Pce>((_state.MemAddrWrite << 1) + 1, value, MemoryType::PceVideoRam);
							_vram[_state.MemAddrWrite] = _state.VramData;
						}

						_state.MemAddrWrite += _state.VramAddrIncrement;
					}
					break;

				case 0x05:
					if(msb) {
						//TODO output select
						//TODO dram refresh
						switch((value >> 3) & 0x03) {
							case 0: _state.VramAddrIncrement = 1; break;
							case 1: _state.VramAddrIncrement = 0x20; break;
							case 2: _state.VramAddrIncrement = 0x40; break;
							case 3: _state.VramAddrIncrement = 0x80; break;
						}
					} else {
						_state.EnableCollisionIrq = (value & 0x01) != 0;
						_state.EnableOverflowIrq = (value & 0x02) != 0;
						_state.EnableScanlineIrq = (value & 0x04) != 0;
						_state.EnableVerticalBlankIrq = (value & 0x08) != 0;
						
						//TODO external sync bits

						_state.NextSpritesEnabled = (value & 0x40) != 0;
						_state.NextBackgroundEnabled = (value & 0x80) != 0;

						if(!_state.BurstModeEnabled) {
							_state.SpritesEnabled = _state.NextSpritesEnabled;
							_state.BackgroundEnabled = _state.NextBackgroundEnabled;
						}
					}
					break;

				case 0x06: UpdateReg<0x3FF>(_state.RasterCompareRegister, value, msb); break;
				case 0x07: UpdateReg<0x3FF>(_state.BgScrollX, value, msb); break;
				case 0x08:
					UpdateReg<0x1FF>(_state.BgScrollY, value, msb);
					UpdateReg<0x1FF>(_state.BgScrollYLatch, value, msb);
					break;

				case 0x09:
					if(!msb) {
						switch((value >> 4) & 0x03) {
							case 0: _state.ColumnCount = 32; break;
							case 1: _state.ColumnCount = 64; break;
							case 2: case 3: _state.ColumnCount = 128; break;
						}

						_state.RowCount = (value & 0x40) ? 64 : 32;

						_state.VramAccessMode = value & 0x03;
						_state.SpriteAccessMode = (value >> 2) & 0x03;
						_state.CgMode = (value & 0x80) != 0;
					}
					break;

				case 0x0A: UpdateReg<0x7F1F>(_state.HorizSync, value, msb); break;
				case 0x0B: UpdateReg<0x7F7F>(_state.HorizDisplay, value, msb); break;
				case 0x0C: 
					if(msb) {
						_state.VertDisplayStart = value;
					} else {
						_state.VertSyncWidth = value & 0x1F;
					}
					break;

				case 0x0D: UpdateReg<0x1FF>(_state.VertDisplayWidth, value, msb); break;

				case 0x0E: _state.VertEndPosVcr = value; break;

				case 0x0F:
					if(!msb) {
						_state.VramSatbIrqEnabled = (value & 0x01) != 0;
						_state.VramVramIrqEnabled = (value & 0x02) != 0;
						_state.DecrementSrc = (value & 0x04) != 0;
						_state.DecrementDst = (value & 0x08) != 0;
						_state.RepeatSatbTransfer = (value & 0x10) != 0;
					}
					break;

				case 0x10: UpdateReg(_state.BlockSrc, value, msb); break;
				case 0x11: UpdateReg(_state.BlockDst, value, msb); break;
				case 0x12:
					UpdateReg(_state.BlockLen, value, msb);
					//todo trigger dma
					MessageManager::Log("vram-vram dma");
					break;

				case 0x13:
					UpdateReg(_state.SatbBlockSrc, value, msb);
					if(msb) {
						_state.SatbTransferPending = true;
					}
					break;
			}
	}
}

uint8_t PcePpu::ReadVce(uint16_t addr)
{
	switch(addr & 0x07) {
		default:
		case 0: return 0xFF; //write-only, reads return $FF
		case 1: return 0xFF; //unused, reads return $FF
		case 2: return 0xFF; //write-only, reads return $FF
		case 3: return 0xFF; //write-only, reads return $FF

		case 4: return _paletteRam[_state.PalAddr] & 0xFF;
		
		case 5: {
			uint8_t val = (_paletteRam[_state.PalAddr] >> 8) & 0x01;
			_state.PalAddr = (_state.PalAddr + 1) & 0x1FF;

			//Bits 1 to 7 are set to 1 when reading MSB
			return 0xFE | val;
		}

		case 6: return 0xFF; //unused, reads return $FF
		case 7: return 0xFF; //unused, reads return $FF
	}
}

void PcePpu::WriteVce(uint16_t addr, uint8_t value)
{
	switch(addr & 0x07) {
		case 0x00:
			//TODO
			LogDebug("[Debug] Write - VCE missing handler: $" + HexUtilities::ToHex(addr) + " = " + HexUtilities::ToHex(value));
			break;

		case 0x01: break; //Unused, writes do nothing

		case 0x02: _state.PalAddr = (_state.PalAddr & 0x100) | value; break;
		case 0x03: _state.PalAddr = (_state.PalAddr & 0xFF) | ((value & 0x01) << 8); break;

		case 0x04:
			_emu->ProcessPpuWrite<CpuType::Pce>((_state.PalAddr << 1), value, MemoryType::PceVideoRam);
			_paletteRam[_state.PalAddr] = (_paletteRam[_state.PalAddr] & 0x100) | value;
			break;

		case 0x05:
			_emu->ProcessPpuWrite<CpuType::Pce>((_state.PalAddr << 1) + 1, value, MemoryType::PceVideoRam);
			_paletteRam[_state.PalAddr] = (_paletteRam[_state.PalAddr] & 0xFF) | ((value & 0x01) << 8);
			_state.PalAddr = (_state.PalAddr + 1) & 0x1FF;
			break;

		case 0x06: break; //Unused, writes do nothing
		case 0x07: break; //Unused, writes do nothing
	}
}