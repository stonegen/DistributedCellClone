/*
 * Generated by asn1c-0.9.29 (http://lionet.info/asn1c)
 * From ASN.1 module "Structures"
 * 	found in "../asn1_schema/structures.asn"
 */

#ifndef	_ENB_StatusTransfer_TransparentContainer_H_
#define	_ENB_StatusTransfer_TransparentContainer_H_


#include <asn_application.h>

/* Including external dependencies */
#include "Bearers-SubjectToStatusTransferList.h"
#include <constr_SEQUENCE.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ENB-StatusTransfer-TransparentContainer */
typedef struct ENB_StatusTransfer_TransparentContainer {
	Bearers_SubjectToStatusTransferList_t	 bearers;
	
	/* Context for parsing across buffer boundaries */
	asn_struct_ctx_t _asn_ctx;
} ENB_StatusTransfer_TransparentContainer_t;

/* Implementation */
extern asn_TYPE_descriptor_t asn_DEF_ENB_StatusTransfer_TransparentContainer;
extern asn_SEQUENCE_specifics_t asn_SPC_ENB_StatusTransfer_TransparentContainer_specs_1;
extern asn_TYPE_member_t asn_MBR_ENB_StatusTransfer_TransparentContainer_1[1];

#ifdef __cplusplus
}
#endif

#endif	/* _ENB_StatusTransfer_TransparentContainer_H_ */
#include <asn_internal.h>