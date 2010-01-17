#pragma once

#include "si_tables.h"
#include "decrypter.h"
#include "pluginshandler.h"
#include "configuration.h"
#include "NetworkProvider.h"

#define MAX_PSI_SECTION_LENGTH	4096

using namespace std;
using namespace stdext;

class DVBParser;

// Abstract class for TS packet parser
class TSPacketParser
{
public:
	// This is the only common function for all derived parsers
	virtual void parseTSPacket(const ts_t* const packet, USHORT pid, bool& abandonPacket) = 0;
};

// Elementary stream types
enum ESType { ES_TYPE_VIDEO, ES_TYPE_AUDIO, ES_TYPE_DVB_SUBTITLE, ES_TYPE_TELETEXT_SUBTITLE, ES_TYPE_OTHER };

// Elementary Stream Information
struct ESInfo
{
	ESType							type;
	string							language;
	BYTE							streamType;
	list<BYTE[256]>					descriptors;
};

// PSI tables parser
class PSIParser : public TSPacketParser
{
private:
	// Auxiliary structure for parsing tables
	struct SectionBuffer
	{
		USHORT							offset;							// Offset of the last write into the buffer
		USHORT							expectedLength;					// Expected length of the section
		BYTE							lastContinuityCounter;			// The value of last continuity counter
		BYTE							buffer[MAX_PSI_SECTION_LENGTH];	// The buffer itself

		SectionBuffer() : offset(0), expectedLength(0), lastContinuityCounter((BYTE)'\xFF') {}	// Constructor (making sure the offset is 0 in the beginning)
	};

	// Routines for handling varous tables parsing
	void parseTable(const pat_t* const table,  short remainingLength, bool& abandonPacket);
	void parsePATTable(const pat_t* const table, short remainingLength, bool& abandonPacket);
	void parsePMTTable(const pmt_t* const table, short remainingLength);
	void parseSDTTable(const sdt_t* const table, short remainingLength);
	void parseBATTable(const nit_t* const table, short remainingLength);
	void parseNITTable(const nit_t* const table, short remainingLength);
	void parseCATTable(const cat_t* const table, short remainingLength);
	void parseUnknownTable(const pat_t* const table, const short remainingLength) const;

	// This is provide-wide data
	NetworkProvider									m_Provider;						// All provider-wide data goes here

	// This is transponder-wide data
	hash_map<USHORT, USHORT>						m_PMTPids;						// SID to PMT PID map
	hash_map<USHORT, hash_set<CAScheme>>			m_CATypesForSid;				// SID to CA types map
	hash_map<USHORT, hash_set<USHORT>>				m_ESPidsForSid;					// SID to ES PIDs map
	hash_map<USHORT, hash_set<USHORT>>				m_CAPidsForSid;					// SID to CA PIDs map
	hash_map<USHORT, SectionBuffer>					m_BufferForPid;					// PID to Table map
	hash_map<USHORT, BYTE>							m_PidType;						// PID to Type map
	USHORT											m_CurrentTID;					// Current transponder TID
	USHORT											m_CurrentONID;					// Current network ONID
	EMMInfo											m_EMMPids;						// EMM PID to EMM CA types map
	
	// Other important data
	USHORT											m_PMTCounter;					// Number of PMT packets encountered before any CAT packet
	DVBParser* const								m_pParent;						// Parent stream parser objects
	bool											m_AllowParsing;					// Becomes false before stopping the graph
	time_t											m_TimeStamp;					// Last update time stamp
	bool											m_ProviderInfoHasBeenCopied;	// True if the tables have been copied to the encoder

	bool											m_AustarDigitalDone;			// True after Austar Digital has been scanned

	// Disallow default and copy constructors
	PSIParser();
	PSIParser(const PSIParser&);

public:
	// Constructor
	PSIParser(DVBParser* const pParent) : 
		m_CurrentTID(0),
		m_CurrentONID(0),
		m_pParent(pParent),
		m_AllowParsing(true),
		m_PMTCounter(0),
		m_TimeStamp(0),
		m_ProviderInfoHasBeenCopied(false),
		m_AustarDigitalDone(false)
	{
	}

	// Query methods
	const NetworkProvider& getNetworkProvider() const							{ return m_Provider; }
	bool getPMTPidForSid(USHORT sid, USHORT& pmtPid) const;
	bool getESPidsForSid(USHORT sid, hash_set<USHORT>& esPids) const;
	bool getCAPidsForSid(USHORT sid, hash_set<USHORT>& caPids) const;
	bool getECMCATypesForSid(USHORT sid, hash_set<CAScheme>& ecmCATypes) const;
	void getEMMCATypes(EMMInfo& emmCATypes) const								{ emmCATypes = m_EMMPids; }
	time_t getTimeStamp() const													{ return m_TimeStamp; }
	bool providerInfoHasBeenCopied() const										{ return m_ProviderInfoHasBeenCopied; }
	USHORT getCurrentTID() const												{ return m_CurrentTID; }
	USHORT getCurrentONID() const												{ return m_CurrentONID; }
	BYTE getTypeForPid(USHORT pid) const;

	// Setter for "Has been copied" flag
	void setProviderInfoHasBeenCopied()											{ m_ProviderInfoHasBeenCopied = true; }

	// Clear method
	void clear();

	// This method disallows parsing just before stopping the graph to avoid buffers corruption
	void disallowParsing()	{ m_AllowParsing = false; }
	// This method allows parsing just before stopping the graph to avoid buffers corruption
	void allowParsing()		{ m_AllowParsing = true; }

	// Overridden parsing method
	virtual void parseTSPacket(const ts_t* const packet, USHORT pid, bool& abandonPacket);
};

class Recorder;

enum KeyCorrectness { KEY_WRONG, KEY_MAYBE_OK, KEY_OK};

// ES and CA packets parser
class ESCAParser : public TSPacketParser
{
	// This structure defines an output buffer to be decrypted together
	struct OutputBuffer
	{
		BYTE* const	buffer;										// The output buffer itself
		ULONG		numberOfPackets;							// Current number of packets in it
		BYTE		oddKey[8];									// CSA odd DCW
		BYTE		evenKey[8];									// CSA even DCW
		bool		hasKey;										// True if has one of the keys
		bool		keyVerified;								// True if the key has been verified

		// Default constructor
		OutputBuffer() : 
			buffer(new BYTE[TS_PACKET_LEN * g_pConfiguration->getTSPacketsPerOutputBuffer()]),
			numberOfPackets(0),
			hasKey(false),
			keyVerified(false)
		{
			// Set the initial values for keys
			ZeroMemory(oddKey, sizeof(oddKey));
			ZeroMemory(evenKey, sizeof(evenKey));
		}

		// Copy constructor
		OutputBuffer(const OutputBuffer& other) : 
			buffer(new BYTE[TS_PACKET_LEN * g_pConfiguration->getTSPacketsPerOutputBuffer()])
		{
			numberOfPackets = other.numberOfPackets;
			hasKey = other.hasKey;
			keyVerified = other.keyVerified;
			memcpy(buffer, other.buffer, TS_PACKET_LEN * g_pConfiguration->getTSPacketsPerOutputBuffer());
			memcpy(oddKey, other.oddKey, sizeof(oddKey));
			memcpy(evenKey, other.evenKey, sizeof(evenKey));
		}

		virtual ~OutputBuffer()
		{
			delete [] buffer;
		}
	};

	// This is the worker thread routine
	friend DWORD WINAPI parserWorkerThreadRoutine(LPVOID param);
private:
	// Put the packet to the output buffer for worker thread (for ES packets)
	void putToOutputBuffer(const BYTE* const packet);
	// Decrypt and write pending packets, called from both main and worker threads
	void decryptAndWritePending(bool immediately);
	// Send the CA packet to the CAM
	void sendToCam(const BYTE* const currentPacket, USHORT caPid);

	// Data members
	FILE* const						m_pOutFile;							// Output file stream
	bool							m_ExitWorkerThread;					// Flag for graceful exitting of worker thread
	Recorder* const					m_pRecorder;						// The owining recorder
	LPCTSTR							m_ChannelName;						// The channel name
	
	// Decryption stuff
	Decrypter						m_Decrypter;						// Decrypter object
	PluginsHandler*	const			m_pPluginsHandler;					// Plugins handler object
	BYTE							m_LastECMPacket[PACKET_SIZE];		// Last new ECM packet

	// Output buffer stuff
	deque<OutputBuffer* const>		m_OutputBuffers;					// Vector of output buffers (one per distinct ECM packet)
	CCritSec						m_csOutputBuffer;					// Critical section on output buffers structure
	HANDLE							m_WorkerThread;						// The worker thread handling decryption
	const bool						m_IsEncrypted;						// True if the ES are encrypted
	__int64							m_FileLength;						// Contains the output file length so far
	const __int64					m_MaxFileLength;					// Max file length for cyclic buffer, for regular file -1
	__int64							m_CurrentPosition;					// Current position in the cyclic buffer, otherwise unused
	USHORT							m_ResetCounter;						// How many subsequent resets have been encountered?

	// Assigned PID differentiator
	hash_map<USHORT, bool>			m_IsESPid;							// PID to bool map, true for ES, false for CA
	hash_map<USHORT, bool>			m_ValidPacketFound;					// PID to bool map, true when a first valid packet was found for an ES PID
	hash_map<USHORT, bool>			m_ShouldBeValidated;				// PID to bool map, true when the packets from this PID should be valid PES packets
	const USHORT					m_Sid;								// SID of the program being recorded
	const USHORT					m_PmtPid;							// PID of PMT of the program being recorded
	const hash_set<CAScheme>		m_ECMCATypes;						// ECM CA types of the program being recorded
	const EMMInfo					m_EMMCATypes;						// EMM PIDs to EMM CA types map

	// Different dilution stuff
	USHORT							m_PATCounter;						// Running PAT packet counter
	USHORT							m_PATContinuityCounter;				// Current PAT continuity counter
	USHORT							m_PMTCounter;						// Running PMT packet counter
	USHORT							m_PMTContinuityCounter;				// Current PMT continuity counter

	// Internal write method to handle cyclic buffers
	int write(const BYTE* const buffer, int bytesToWrite);

	// Check audio language in ES descriptor
	static bool matchAudioLanguage(const BYTE* const buffer, const int bufferLength, const char* language);

	// Check if a key can decrypt the current buffer
	KeyCorrectness isCorrectKey(const BYTE* const buffer, ULONG numberOfPackets, const BYTE* const oddKey, bool checkAgainstOddKey,	const BYTE* const evenKey, bool checkAgainstEvenKey);

	// Default and copy constructors are disallowed
	ESCAParser();
	ESCAParser(const ESCAParser&);

public:
	// The only valid constructor
	ESCAParser(Recorder* const pRecorder,
			   FILE* const pFile,
			   PluginsHandler* const pPluginsHandler,
			   LPCTSTR channelName,
			   USHORT sid,
			   USHORT pmtPid,
			   const hash_set<CAScheme>& ecmCATypes,
			   const EMMInfo& emmCATypes,
			   __int64 maxFileLength);

	// Destructor
	virtual ~ESCAParser();

	// Overridden parsing method
	virtual void parseTSPacket(const ts_t* const packet, USHORT pid, bool& abandonPacket);

	// This routine is called by the plugins handler
	bool setKey(bool isOddKey, const BYTE* const key, bool setWithNoCheck);

	// Tell PID meaning
	void setESPid(USHORT pid, bool isESPid);

	// Tell is the PID should be validated
	void setValidatePid(USHORT pid, bool validate)								{ m_ShouldBeValidated[pid] = validate; }

	// Reset the parser
	void reset();

	// Get the file length
	__int64 getFileLength() const												{ return m_FileLength; }

	// Get the tuner ordinal
	int getTunerOrdinal() const;

	// Returns true if a type should be validated
	static bool validateType(BYTE type)											{ return (type >= (BYTE)0x01 && type <=(BYTE)0x04) || type == (BYTE)0x06; }
};

// TS stream parser
class DVBParser
{
private:
	// These are to handle augmented TS packets
	long	m_LeftoverLength;
	BYTE	m_LeftoverBuffer[TS_PACKET_LEN];

	// Stream PID to Parser map
	hash_map<USHORT, hash_set<TSPacketParser*>>	m_ParserForPid;

	// We have only one PSI parser
	PSIParser m_PSIParser;

	// Critical section for locking and unlocking
	CCritSec	m_cs;

	// Flag saying no clients connected yet
	bool		m_HasConnectedClients;

	// Tuner ordinal number
	const UINT	m_TunerOrdinal;

	// Dump file (created only on request)
	FILE* m_FullTransponderFile;

	// Disallow default and copy constructor
	DVBParser();
	DVBParser(const DVBParser&);

public:
	// The only available constructor
	DVBParser(UINT ordinal) :	
	  m_PSIParser(this),	m_HasConnectedClients(false), m_LeftoverLength(0), m_TunerOrdinal(ordinal),	m_FullTransponderFile(NULL) {}
	
	// Destructor
	virtual ~DVBParser()															{ stopTransponderDump(); }

	// Tuner ordinal getter
	UINT getTunerOrdinal() const													{ return m_TunerOrdinal; }

	// Reset the parser map
	void resetParser(bool clearPSIParser);

	// Assign parser to a specific PID
	void assignParserToPid(USHORT pid, TSPacketParser* parser);

	// Stop listening on a specific parser
	void dropParser(TSPacketParser* parser);

	// Generic function for parsing a bunch of TS packets
	void parseTSStream(const BYTE* inputBuffer, int inputBufferLength);	

	// Query methods - delegated to the internal PSI parser
	const NetworkProvider& getNetworkProvider() const								{ return m_PSIParser.getNetworkProvider(); }
	bool getPMTPidForSid(USHORT sid, USHORT& pmtPid) const							{ return m_PSIParser.getPMTPidForSid(sid, pmtPid); }
	bool getESPidsForSid(USHORT sid, hash_set<USHORT>& esPids) const				{ return m_PSIParser.getESPidsForSid(sid, esPids); }
	bool getCAPidsForSid(USHORT sid, hash_set<USHORT>& caPids) const				{ return m_PSIParser.getCAPidsForSid(sid, caPids); }
	bool getECMCATypesForSid(USHORT sid, hash_set<CAScheme>& ecmCATypes) const		{ return m_PSIParser.getECMCATypesForSid(sid, ecmCATypes); }
	void getEMMCATypes(EMMInfo& emmCATypes) const									{ m_PSIParser.getEMMCATypes(emmCATypes); }
	time_t getTimeStamp() const														{ return m_PSIParser.getTimeStamp(); }
	bool providerInfoHasBeenCopied() const											{ return m_PSIParser.providerInfoHasBeenCopied(); }
	USHORT getCurrentTID() const													{ return m_PSIParser.getCurrentTID(); }
	USHORT getCurrentONID() const													{ return m_PSIParser.getCurrentONID(); }
	BYTE getTypeForPid(USHORT pid) const											{ return m_PSIParser.getTypeForPid(pid); }

	// Setter for the internal parser "HasBeenCopied" flag
	void setProviderInfoHasBeenCopied()												{ m_PSIParser.setProviderInfoHasBeenCopied(); }

	// Lock and unlock
	void lock();
	void unlock();

	// This method stop PSI packets parsing just before stopping the graph to avoid corruption
	void stopPSIPackets()															{ m_PSIParser.disallowParsing(); }
	// This method resumes PSI packets parsing just before stopping the graph to avoid corruption
	void resumePSIPackets()															{ m_PSIParser.allowParsing(); }

	// Get HasConnectedClients flag
	bool getHasConnectedClients() const												{ return m_HasConnectedClients; }

	// Set m_HasConnectedClients flag
	void setHasConnectedClients()													{ m_HasConnectedClients = true; }

	// Start dumping the full transponder
	void startTransponderDump();

	// Stop dumping the full transponder
	void stopTransponderDump();
};
