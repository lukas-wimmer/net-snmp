/*
 * Note: this file originally auto-generated by mib2c using
 *       version : 1.10 $ of : mfd-data-access.m2c,v $
 *
 * $Id$
 */
#ifndef INETNETTOMEDIATABLE_DATA_ACCESS_H
#define INETNETTOMEDIATABLE_DATA_ACCESS_H

#ifdef __cplusplus
extern          "C" {
#endif


    /*
     *********************************************************************
     * function declarations
     */

    /*
     *********************************************************************
     * Table declarations
     */
/**********************************************************************
 **********************************************************************
 ***
 *** Table inetNetToMediaTable
 ***
 **********************************************************************
 **********************************************************************/
    /*
     * inetNetToMediaTable is subid 35 of ip.
     * It's status is Current.
     * OID: .1.3.6.1.2.1.4.35, length: 8
     */


    int            
        inetNetToMediaTable_init_data(inetNetToMediaTable_registration_ptr
                                      inetNetToMediaTable_reg);


    void            inetNetToMediaTable_container_init(netsnmp_container **
                                                       container_ptr_ptr,
                                                       netsnmp_cache *
                                                       cache);
    int             inetNetToMediaTable_cache_load(netsnmp_container *
                                                   container);
    void            inetNetToMediaTable_cache_free(netsnmp_container *
                                                   container);

    int            
        inetNetToMediaTable_row_prep(inetNetToMediaTable_rowreq_ctx *
                                     rowreq_ctx);


#ifdef __cplusplus
}
#endif

#endif                          /* INETNETTOMEDIATABLE_DATA_ACCESS_H */
