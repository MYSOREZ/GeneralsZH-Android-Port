/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// GXCrcStream.h /////////////////////////////////////////////////////////////
// Locate where two machines' lockstep checksums part, from one machine's data.
//
// A replay recorded on the PC client carries that machine's whole-state
// checksum and nothing else: one number per interval. When it disagrees with
// ours, the number says the simulations differ and says nothing about where,
// which is why this port has spent weeks diffing candidate subsystems.
//
// It does not have to be that way. The checksum is built one 32-bit word at a
// time by XferCRC::addCRC, and that step is exactly invertible:
//
//     forward:  crc' = ROL(crc, 1) + word        (mod 2^32)
//     inverse:  crc  = ROR(crc' - word, 1)
//
// So if we keep the words this machine fed in, and the running value after
// each, we can start from the OTHER machine's final number and walk backwards
// through our own words. Wherever the backward walk meets our forward values,
// the two machines agreed from that point to the end -- so the last agreeing
// position is where the difference enters. Labels recorded alongside turn that
// position into "object id 159, 6 words into its contribution".
//
// This reads the PC's number and our own state. It needs no instrumentation on
// the PC, no Windows build, and no second device.
//
// It assumes the two word streams have the same length and differ in one
// region. If the streams are different lengths -- a different object count, a
// module one side does not have -- no position matches, and saying so is itself
// the finding, so report() states that case rather than guessing.

#pragma once

#include "Lib/BaseType.h"

namespace GXCrcStream
{
	// Start capturing. Cheap no-op unless capture is enabled.
	void begin( UnsignedInt frame );

	// Called by XferCRC for every word that enters the checksum.
	void push( UnsignedInt word, UnsignedInt crcAfter );

	// Label the current position. The string is copied.
	void mark( const char *label );
	void markObject( UnsignedInt objectId, const char *templateName );

	// Stop capturing and keep the buffer for report().
	void end();

	// Walk backwards from theirCRC and print where the streams part.
	// theirCRC is the value as it appears in a replay, i.e. after getCRC()'s
	// byte swap, the same form ours is compared in.
	void report( UnsignedInt theirCRC, UnsignedInt ourCRC );

	// For the live path, where the two values being compared are two players'
	// checksums and which one is this machine's is not to hand. Whichever a
	// captured stream ends on is ours; this works that out and reports once.
	void reportEither( UnsignedInt crcA, UnsignedInt crcB );

	// Dump one named section's words, once, so the analysis can move off the phone.
	//
	// A single-word test is all the locator can do, and a rounding difference inside
	// a transform touches twelve words at once, so it cannot see one. Searching for a
	// multi-word difference needs the words themselves. This prints the section's
	// words in hex, its start and end running values, and -- the part that makes the
	// rest computable -- the inverse walk of the OTHER machine's checksum back to the
	// section's end. From those, every intermediate value on both sides inside the
	// section can be reconstructed offline, and any hypothesis about a run of words
	// tested without another build.
	void dumpSection( const char *sectionLabel, UnsignedInt theirCRC, UnsignedInt ourCRC );

	// True while capture is armed, so callers can skip building labels.
	Bool isCapturing();
}
