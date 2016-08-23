#ifndef MOVE_TRANSCEIVER_C
#define MOVE_TRANSCEIVER_C

#ifdef __cplusplus
extern "C"
{
#endif

struct CTransceiver;
typedef struct CTransceiver CTransceiver;

// Struct containing visible parts of the NanoLink header and the payload data.
typedef struct
{
	int arq; // 0/1; 1 for more reliable NanoLink (only works for channel 0..6)
	int channel; // 0..7; channel 7 used for telemtry ring-buffer (only tx)
	const void* data;
	int length;
} transceiver_msg;

// Type for the callback method. When a packet is received, a method of this type is called.
typedef void (*CTransceiver_CallbackRecv) (const transceiver_msg msg);

CTransceiver* CTransceiver_create(const char* spiDevice, const char* interruptPinId);
void CTransceiver_setCallbackRecv(CTransceiver* obj, CTransceiver_CallbackRecv cb);
void CTransceiver_init(CTransceiver* obj);

// Enqueues a packet for transmission.
void CTransceiver_sendFrame(CTransceiver* obj, transceiver_msg msg);

void CTransceiver_destroy(CTransceiver* obj);

#ifdef __cplusplus
}
#endif

#endif // MOVE_TRANSCEIVER_C
