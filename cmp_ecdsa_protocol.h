#include "primitives.h"

#ifndef __CMP20_ECDSA_MPC_PROTOCOL_H__
#define __CMP20_ECDSA_MPC_PROTOCOL_H__

#define KAPPA_RANDOM_ORACLE_BYTES 64      // RO input and output

typedef uint8_t hash_chunk[KAPPA_RANDOM_ORACLE_BYTES];

// super_sesion_id is required for every phases of the protocol
typedef struct 
{
  uint64_t id;
  hash_chunk srid;

  ec_group_t ec;
  gr_elem_t ec_gen;
  scalar_t ec_order;

  uint64_t num_parties;
  uint64_t *parties_ids;

  uint8_t  *bytes;
  uint64_t byte_len;

} cmp_session_id_t;

typedef struct 
{
  scalar_t  secret_x;
  gr_elem_t public_X;

  scalar_t        tau;
  zkp_schnorr_t   *psi;
  zkp_aux_info_t  *aux;

  hash_chunk srid;
  hash_chunk u;
  hash_chunk V;
  hash_chunk echo_broadcast;

  uint64_t run_time;
} cmp_key_generation_t;

typedef struct 
{
  paillier_private_key_t  *paillier_priv;
  ring_pedersen_private_t *rped_priv;

  scalar_t  *reshare_secret_x_j;
  scalar_t  *encrypted_reshare_j;
  gr_elem_t *reshare_public_X_j;
  
  zkp_aux_info_t              *aux;
  scalar_t                    *tau;
  zkp_schnorr_t               **psi_sch;
  zkp_paillier_blum_modulus_t *psi_mod;
  zkp_ring_pedersen_param_t   *psi_rped;

  hash_chunk rho;
  hash_chunk combined_rho;
  hash_chunk u;
  hash_chunk V;
  hash_chunk echo_broadcast;

  uint64_t prime_time;
  uint64_t run_time;
} cmp_refresh_aux_info_t;

typedef struct cmp_party_t
{
  cmp_session_id_t *sid;

  uint64_t id;
  uint64_t index;
  uint64_t num_parties;

  scalar_t  secret_x;                       // private key share
  gr_elem_t *public_X;                      // public key shares of all partys (by index)

  paillier_private_key_t *paillier_priv;
  paillier_public_key_t  **paillier_pub;   
  ring_pedersen_public_t **rped_pub;

  cmp_key_generation_t    *key_generation_data;
  cmp_refresh_aux_info_t  *refresh_data;

  struct cmp_party_t **parties;             // Access all parties, to get their info, instead of communication channels
} cmp_party_t;


void cmp_sample_bytes (uint8_t *rand_byte, uint64_t byte_len);

cmp_session_id_t *
      cmp_session_id_new            (uint64_t id, uint64_t num_parties, uint64_t *partys_ids);
void  cmp_session_id_free           (cmp_session_id_t *ssid);
void  cmp_session_id_append_bytes   (cmp_session_id_t *sid, const uint8_t *data, uint64_t data_len);

void  cmp_party_new   (cmp_party_t **parties, uint64_t num_parties, uint64_t index, uint64_t id, cmp_session_id_t *ssid);
void  cmp_party_free  (cmp_party_t *party);

void  cmp_key_generation_init         (cmp_party_t *party);
void  cmp_key_generation_clean        (cmp_party_t *party);
void  cmp_key_generation_round_1_exec (cmp_party_t *party);
void  cmp_key_generation_round_2_exec (cmp_party_t *party);
void  cmp_key_generation_round_3_exec (cmp_party_t *party);
void  cmp_key_generation_final_exec   (cmp_party_t *party);

void  cmp_refresh_aux_info_init         (cmp_party_t *party);
void  cmp_refresh_aux_info_clean        (cmp_party_t *party);
void  cmp_refresh_aux_info_round_1_exec (cmp_party_t *party);
void  cmp_refresh_aux_info_round_2_exec (cmp_party_t *party);
void  cmp_refresh_aux_info_round_3_exec (cmp_party_t *party);
void  cmp_refresh_aux_info_final_exec   (cmp_party_t *party);

#endif