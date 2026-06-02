// Copyright getnamo. NodeJs-Unreal v2.0.0
//
// Self-delimiting, length-prefixed frame protocol shared between the C++ side
// (this codec) and the node-side process.js bridge. The bridge runs in bytes
// mode (CLISystem bProcessInBytes), so every message - logs, actions, events,
// control commands - is wrapped in a frame on the single stdio byte stream.
//
// Wire format (all integers little-endian):
//   [4] MAGIC      = 0x4E 0x55 0x45 0x01  ('N','U','E',0x01)
//   [1] TYPE
//   [4] HEADER_LEN (uint32)
//   [HEADER_LEN] HEADER  (UTF-8)
//   [4] BINARY_LEN (uint32)
//   [BINARY_LEN] BINARY  (may be 0)
//
// EVENT frames carry, in their BINARY field, a "binary table" of N buffers so a
// single event can interweave multiple binary blobs alongside its JSON args.
// Table format: [4] count, then count * ( [4] len, [len] bytes ).

#pragma once

#include "CoreMinimal.h"

namespace ENodeFrameType
{
	enum Type : uint8
	{
		Log        = 0x01, // node->UE  : script console.log text
		Action     = 0x02, // node->UE  : "begin|end|reload <scriptPath>"
		Event      = 0x03, // both ways : JSON {script,name,args} + binary table
		Error      = 0x04, // node->UE  : JSON {script,message,stack}
		Control    = 0x05, // UE->node  : command line text
		ProcessLog = 0x06, // node->UE  : process-level (wrapper) log text
		Npm        = 0x07, // node->UE  : JSON {installed:bool, error:string}
	};
}

/**
 * Stateful frame codec. Encode is static; decoding is incremental via Feed()
 * which tolerates partial frames split across reads and resynchronises on the
 * magic marker if the stream is ever corrupted.
 */
class NODEJS_API FNodeFrameCodec
{
public:
	static const uint8 Magic[4];

	/** Encode a single frame (with optional binary payload). */
	static TArray<uint8> Encode(uint8 Type, const FString& Header, const TArray<uint8>& Binary);
	static TArray<uint8> Encode(uint8 Type, const FString& Header);

	/** Build/parse the binary table used inside EVENT frames. */
	static TArray<uint8> BuildBinaryTable(const TArray<TArray<uint8>>& Buffers);
	static bool ParseBinaryTable(const TArray<uint8>& Table, TArray<TArray<uint8>>& OutBuffers);

	/** Feed raw bytes from the pipe; complete frames are emitted via OnFrame. */
	void Feed(const TArray<uint8>& Chunk);

	/** Called once per fully-decoded frame (on the calling thread of Feed). */
	TFunction<void(uint8 Type, const FString& Header, const TArray<uint8>& Binary)> OnFrame;

private:
	TArray<uint8> Accum;

	void TryParse();
	bool MatchMagicAt(int32 Index) const;
	int32 FindMagicFrom(int32 Start) const;
};
