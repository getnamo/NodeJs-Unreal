// Copyright getnamo. NodeJs-Unreal v2.0.0

#include "NodeFrameCodec.h"

const uint8 FNodeFrameCodec::Magic[4] = { 0x4E, 0x55, 0x45, 0x01 };

namespace
{
	FORCEINLINE void WriteU32(TArray<uint8>& Out, uint32 Value)
	{
		Out.Add((uint8)(Value & 0xFF));
		Out.Add((uint8)((Value >> 8) & 0xFF));
		Out.Add((uint8)((Value >> 16) & 0xFF));
		Out.Add((uint8)((Value >> 24) & 0xFF));
	}

	FORCEINLINE uint32 ReadU32(const TArray<uint8>& In, int32 Offset)
	{
		return (uint32)In[Offset]
			| ((uint32)In[Offset + 1] << 8)
			| ((uint32)In[Offset + 2] << 16)
			| ((uint32)In[Offset + 3] << 24);
	}

	// Header byte size of a frame up to (but excluding) the header payload.
	// magic(4) + type(1) + headerLen(4)
	constexpr int32 PreHeaderSize = 9;
}

TArray<uint8> FNodeFrameCodec::Encode(uint8 Type, const FString& Header, const TArray<uint8>& Binary)
{
	FTCHARToUTF8 HeaderUtf8(*Header);

	TArray<uint8> Out;
	Out.Reserve(PreHeaderSize + HeaderUtf8.Length() + 4 + Binary.Num());

	Out.Append(Magic, 4);
	Out.Add(Type);
	WriteU32(Out, (uint32)HeaderUtf8.Length());
	Out.Append((const uint8*)HeaderUtf8.Get(), HeaderUtf8.Length());
	WriteU32(Out, (uint32)Binary.Num());
	if (Binary.Num() > 0)
	{
		Out.Append(Binary.GetData(), Binary.Num());
	}
	return Out;
}

TArray<uint8> FNodeFrameCodec::Encode(uint8 Type, const FString& Header)
{
	static const TArray<uint8> Empty;
	return Encode(Type, Header, Empty);
}

TArray<uint8> FNodeFrameCodec::BuildBinaryTable(const TArray<TArray<uint8>>& Buffers)
{
	TArray<uint8> Out;
	WriteU32(Out, (uint32)Buffers.Num());
	for (const TArray<uint8>& Buf : Buffers)
	{
		WriteU32(Out, (uint32)Buf.Num());
		Out.Append(Buf.GetData(), Buf.Num());
	}
	return Out;
}

bool FNodeFrameCodec::ParseBinaryTable(const TArray<uint8>& Table, TArray<TArray<uint8>>& OutBuffers)
{
	OutBuffers.Reset();
	if (Table.Num() == 0)
	{
		return true; // empty table is valid (no binary args)
	}
	if (Table.Num() < 4)
	{
		return false;
	}

	int32 Cursor = 0;
	const uint32 Count = ReadU32(Table, Cursor);
	Cursor += 4;

	for (uint32 i = 0; i < Count; ++i)
	{
		if (Cursor + 4 > Table.Num())
		{
			return false;
		}
		const uint32 Len = ReadU32(Table, Cursor);
		Cursor += 4;

		if (Cursor + (int32)Len > Table.Num())
		{
			return false;
		}
		TArray<uint8>& Buf = OutBuffers.AddDefaulted_GetRef();
		Buf.Append(Table.GetData() + Cursor, Len);
		Cursor += Len;
	}
	return true;
}

void FNodeFrameCodec::Feed(const TArray<uint8>& Chunk)
{
	Accum.Append(Chunk);
	TryParse();
}

bool FNodeFrameCodec::MatchMagicAt(int32 Index) const
{
	if (Index + 4 > Accum.Num())
	{
		return false;
	}
	return Accum[Index] == Magic[0]
		&& Accum[Index + 1] == Magic[1]
		&& Accum[Index + 2] == Magic[2]
		&& Accum[Index + 3] == Magic[3];
}

int32 FNodeFrameCodec::FindMagicFrom(int32 Start) const
{
	for (int32 i = Start; i + 4 <= Accum.Num(); ++i)
	{
		if (MatchMagicAt(i))
		{
			return i;
		}
	}
	return INDEX_NONE;
}

void FNodeFrameCodec::TryParse()
{
	int32 Cursor = 0;

	while (true)
	{
		// Need enough to read the magic + type + header length.
		if (Accum.Num() - Cursor < PreHeaderSize)
		{
			break;
		}

		// Resync to the magic marker if we're not aligned to a frame start.
		if (!MatchMagicAt(Cursor))
		{
			const int32 Found = FindMagicFrom(Cursor + 1);
			if (Found == INDEX_NONE)
			{
				// No frame start in view. Drop everything but a possible
				// partial magic tail (last 3 bytes).
				Cursor = FMath::Max(Cursor, Accum.Num() - 3);
				break;
			}
			Cursor = Found;
			continue;
		}

		int32 P = Cursor + 4;
		const uint8 Type = Accum[P];
		P += 1;

		const uint32 HeaderLen = ReadU32(Accum, P);
		P += 4;

		// Wait for the full header + the binary length field.
		if (Accum.Num() < P + (int32)HeaderLen + 4)
		{
			break;
		}

		FString Header;
		if (HeaderLen > 0)
		{
			FUTF8ToTCHAR Conv((const ANSICHAR*)(Accum.GetData() + P), (int32)HeaderLen);
			Header = FString(Conv.Length(), Conv.Get());
		}
		P += HeaderLen;

		const uint32 BinaryLen = ReadU32(Accum, P);
		P += 4;

		// Wait for the full binary payload.
		if (Accum.Num() < P + (int32)BinaryLen)
		{
			break;
		}

		TArray<uint8> Binary;
		if (BinaryLen > 0)
		{
			Binary.Append(Accum.GetData() + P, BinaryLen);
		}
		P += BinaryLen;

		if (OnFrame)
		{
			OnFrame(Type, Header, Binary);
		}

		Cursor = P;
	}

	if (Cursor > 0)
	{
		Accum.RemoveAt(0, Cursor, EAllowShrinking::No);
	}
}
