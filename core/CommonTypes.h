/*
 * s3tp_types.h
 *
 *  Created on: Aug 11, 2016
 *      Author: dai
 */

#ifndef CORE_S3TP_TYPES_H_
#define CORE_S3TP_TYPES_H_

#include <stdlib.h>
#include "constants.h"

typedef int SOCKET;

#pragma pack(push, 1)
typedef struct tag_s3tp_header
{
	uint16_t crc;
	uint16_t seq;		/* Global Sequence Number */
				/* First Byte: Seq_head, 
				 * 2nd Byte: Seq_sub 
				 used for packet fragmentation 
				*/
	/*
	 * Contains the length of the payload of this current packet.
	 * The last 2 bits are reserved for the protocol.
	 */
	uint16_t pdu_length;
	uint8_t seq_port;		/* used for reordering */
    uint8_t port;

	//Fragmentation bit functions (bit is the most significant bit of the port variable)
	uint8_t moreFragments() {
		return (uint8_t)((port >> 7) & 1);
	}

	void setMoreFragments() {
		port |= 1 << 7;
	}

	void unsetMoreFragments() {
		port &= ~(1 << 7);
	}

	//Getters and setters
	uint8_t getPort() {
		return port & (uint8_t)0x7F;
	}

	void setPort(uint8_t port) {
		uint8_t fragFlag = moreFragments();
		this->port = (fragFlag << 7) | port;
	}

	uint8_t getGlobalSequence() {
		return (uint8_t) (seq >> 8);
	}

	void setGlobalSequence(uint8_t global_seq) {
		seq = (uint16_t )((seq & 0xFF) | (global_seq << 8));
	}

	uint8_t getSubSequence() {
		return (uint8_t) (seq & 0xFF);
	}

	void setSubSequence(uint8_t sub_seq) {
		seq = (uint16_t)((seq & 0xFF00) | sub_seq);
	}

    uint16_t getPduLength() {
        return (uint16_t )(pdu_length & 0x3FFF);
    }

    void setPduLength(uint16_t pdu_len) {
        pdu_length = (uint16_t)((pdu_length & 0x4000) | pdu_len);
    }

	uint8_t getMessageType() {
		return (uint8_t)(pdu_length >> 14);
	}

	void setMessageType(uint8_t type) {
        pdu_length = (uint16_t )((pdu_length & 0x3FFF) | (type << 14));
	}
}S3TP_HEADER;

/**
 * Structure containing an S3TP packet, made up of an S3TP header and its payload.
 * The underlying buffer is given by a char array, in which the first sizeof(S3TP_HEADER) bytes are the header,
 * while all the following bytes are part of the payload.
 *
 * The structure furthermore contains metadata needed by the protocol, such as options and the virtual channel.
 */
struct S3TP_PACKET{
	char * packet;
	uint8_t channel;
	uint8_t options;

	S3TP_PACKET(const char * pdu, uint16_t pduLen) {
		packet = new char[sizeof(S3TP_HEADER) + (pduLen * sizeof(char))];
		memcpy(getPayload(), pdu, pduLen);
		S3TP_HEADER * header = getHeader();
		header->setPduLength(pduLen);
	}

	int getLength() {
		return (sizeof(S3TP_HEADER) + (getHeader()->getPduLength() * sizeof(char)));
	}

	char * getPayload() {
		return packet + (sizeof(S3TP_HEADER));
	}

	S3TP_HEADER * getHeader() {
		return (S3TP_HEADER *)packet;
	}
};

/*typedef struct tag_s3tp_PACKET {
	S3TP_HEADER hdr;
	char pdu[LEN_S3TP_PDU];
}S3TP_PACKET;*/

#pragma pack(pop)

typedef struct tag_queue_data
{
	S3TP_PACKET * pkt;
	uint8_t channel;			/* Logical Channel to be used on the SPI interface */
	uint8_t options;
}S3TP_PACKET_WRAPPER;

#endif /* CORE_S3TP_TYPES_H_ */
