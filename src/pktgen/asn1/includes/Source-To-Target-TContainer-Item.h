/*
 * Generated by asn1c-0.9.29 (http://lionet.info/asn1c)
 * From ASN.1 module "Structures"
 * 	found in "../asn1_schema/structures.asn"
 */

#ifndef	_Source_To_Target_TContainer_Item_H_
#define	_Source_To_Target_TContainer_Item_H_


#include <asn_application.h>

/* Including external dependencies */
#include "ProtocolIE-ID.h"
#include "Criticality.h"
#include "Source-To-Target-TContainer.h"
#include <constr_SEQUENCE.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Source-To-Target-TContainer-Item */
typedef struct Source_To_Target_TContainer_Item {
	ProtocolIE_ID_t	 id;
	Criticality_t	 criticality;
	Source_To_Target_TContainer_t	 value;
	
	/* Context for parsing across buffer boundaries */
	asn_struct_ctx_t _asn_ctx;
} Source_To_Target_TContainer_Item_t;

/* Implementation */
extern asn_TYPE_descriptor_t asn_DEF_Source_To_Target_TContainer_Item;
extern asn_SEQUENCE_specifics_t asn_SPC_Source_To_Target_TContainer_Item_specs_1;
extern asn_TYPE_member_t asn_MBR_Source_To_Target_TContainer_Item_1[3];

#ifdef __cplusplus
}
#endif

#endif	/* _Source_To_Target_TContainer_Item_H_ */
#include <asn_internal.h>
