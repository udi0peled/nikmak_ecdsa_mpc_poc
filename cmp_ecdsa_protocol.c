#include "common.h"
#include "cmp_ecdsa_protocol.h"
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <semaphore.h>

#define ERR_STR "\nXXXXX ERROR XXXXX\n\n"
extern int PRINT_VALUES;

/********************************************
 *
 *   Communication Channels
 * 
 ********************************************/

#define COMM_CHNL_PATTERN "CHANNEL_%lu_to_%lu_round_%lu.dat"

void cmp_comm_send_bytes(uint64_t my_index, uint64_t to_index, uint64_t round, const uint8_t *bytes, uint64_t byte_len)
{
  char filename[sizeof(COMM_CHNL_PATTERN) + 8];
  sprintf(filename, COMM_CHNL_PATTERN, my_index, to_index, round);

  // Lock reader until finished sending/writing to file
  sem_t* semptr = sem_open(filename, O_CREAT, 0644, 0);

  int fd = open(filename, O_RDWR | O_CREAT, 0644);
  write(fd, bytes, byte_len);

  srand(time(0));
  sleep(rand() % 5 + 1);

  close(fd);
  sem_post(semptr);
  sem_close(semptr);
}

void cmp_comm_receive_bytes(uint64_t from_index, uint64_t my_index, uint64_t round, uint8_t *bytes, uint64_t byte_len)
{
  char filename[sizeof(COMM_CHNL_PATTERN) + 8];
  sprintf(filename, COMM_CHNL_PATTERN, from_index, my_index, round);

  // Wait until file is written by sender
  sem_t* semptr = sem_open(filename, O_CREAT, 0644, 0);
  sem_wait(semptr);

  int fd = open(filename, O_RDONLY, 0644);  
  read(fd, bytes, byte_len);
  close(fd);
  remove(filename);
  sem_close(semptr);
  sem_unlink(filename);
}

/********************************************
 *
 *   Party Context for Protocol Execution
 * 
 ********************************************/

void cmp_sample_bytes (uint8_t *rand_bytes, uint64_t byte_len)
{
  RAND_bytes(rand_bytes, byte_len);
}

/********************************************
 *
 *   Party Context for Protocol Execution
 * 
 ********************************************/

void cmp_set_sid_hash(cmp_party_t *party)
{
  SHA512_CTX sha_ctx;
  SHA512_Init(&sha_ctx);
  SHA512_Update(&sha_ctx, party->sid, sizeof(hash_chunk));
  SHA512_Update(&sha_ctx, party->srid, sizeof(hash_chunk));

  uint8_t *temp_bytes = malloc(PAILLIER_MODULUS_BYTES);           // Enough for uint64_t and group_element

  group_elem_to_bytes(&temp_bytes, GROUP_ELEMENT_BYTES, party->ec_gen, party->ec, 0);
  SHA512_Update(&sha_ctx, temp_bytes, GROUP_ELEMENT_BYTES);

  scalar_to_bytes(&temp_bytes,GROUP_ORDER_BYTES, party->ec_order, 0);
  SHA512_Update(&sha_ctx, temp_bytes, GROUP_ORDER_BYTES);

  for (uint64_t i = 0; i < party->num_parties; ++i)
  {
    SHA512_Update(&sha_ctx, &party->parties_ids[i], sizeof(uint64_t));
    if (party->public_X[i])
    {
      group_elem_to_bytes(&temp_bytes, GROUP_ELEMENT_BYTES, party->public_X[i], party->ec, 0);
      SHA512_Update(&sha_ctx, temp_bytes, GROUP_ELEMENT_BYTES);
    }
    if (party->paillier_pub[i] && party->rped_pub[i])
    {
      scalar_to_bytes(&temp_bytes, PAILLIER_MODULUS_BYTES, party->paillier_pub[i]->N, 0);
      SHA512_Update(&sha_ctx, temp_bytes, PAILLIER_MODULUS_BYTES);
      scalar_to_bytes(&temp_bytes, RING_PED_MODULUS_BYTES, party->rped_pub[i]->N, 0);
      SHA512_Update(&sha_ctx, temp_bytes, RING_PED_MODULUS_BYTES);
      scalar_to_bytes(&temp_bytes, RING_PED_MODULUS_BYTES, party->rped_pub[i]->s, 0);
      SHA512_Update(&sha_ctx, temp_bytes, RING_PED_MODULUS_BYTES);
      scalar_to_bytes(&temp_bytes, RING_PED_MODULUS_BYTES, party->rped_pub[i]->t, 0);
      SHA512_Update(&sha_ctx, temp_bytes, RING_PED_MODULUS_BYTES);
    }
  }
  free(temp_bytes);

  SHA512_Final(party->sid_hash, &sha_ctx);
}

void cmp_party_new (cmp_party_t **parties, uint64_t num_parties, const uint64_t *parties_ids, uint64_t index, const hash_chunk sid)
{
  cmp_party_t *party = malloc(sizeof(*party));
  
  parties[index] = party;
  party->parties = parties;
  
  party->id = parties_ids[index];
  party->index = index;
  party->num_parties = num_parties;
  party->parties_ids = calloc(num_parties, sizeof(uint64_t));
  
  party->secret_x = scalar_new();
  party->public_X = calloc(num_parties, sizeof(gr_elem_t));

  party->paillier_priv = NULL;
  party->paillier_pub  = calloc(num_parties, sizeof(paillier_public_key_t *));
  party->rped_pub      = calloc(num_parties, sizeof(ring_pedersen_public_t *));
  
  party->ec       = ec_group_new();
  party->ec_gen   = ec_group_generator(party->ec);
  party->ec_order = ec_group_order(party->ec);

  party->R   = group_elem_new(party->ec);
  party->k   = scalar_new();
  party->chi = scalar_new();

  // The initialization of the following infulences sid_hash, so init to NULL untill set later.
  memcpy(party->sid, sid, sizeof(hash_chunk));
  memset(party->srid, 0x00, sizeof(hash_chunk));
  for (uint64_t i = 0; i < num_parties; ++i)
  {
    party->parties_ids[i]  = parties_ids[i];
    party->public_X[i]     = NULL;
    party->paillier_pub[i] = NULL;
    party->rped_pub[i]     = NULL; 
  }
  cmp_set_sid_hash(party);

  party->key_generation_data   = NULL;
  party->refresh_data = NULL;
  party->presigning_data = NULL;
}

void cmp_party_free (cmp_party_t *party)
{
  for (uint64_t i = 0; i < party->num_parties; ++i)
  {
    group_elem_free(party->public_X[i]);
    paillier_encryption_free_keys(NULL, party->paillier_pub[i]);
    ring_pedersen_free_param(NULL, party->rped_pub[i]);
  }
  paillier_encryption_free_keys(party->paillier_priv, NULL); 

  group_elem_free(party->R);
  scalar_free(party->k);
  scalar_free(party->chi);
  scalar_free(party->secret_x);
  ec_group_free(party->ec);
  free(party->paillier_pub);
  free(party->parties_ids);
  free(party->rped_pub);
  free(party->public_X);
  free(party);
}

/*********************** 
 * 
 *    Key Generation
 * 
 ***********************/

void cmp_key_generation_init(cmp_party_t *party)
{
  cmp_key_generation_t *kgd = malloc(sizeof(*party->key_generation_data));
  party->key_generation_data = kgd;

  kgd->secret_x = scalar_new();
  kgd->public_X = group_elem_new(party->ec);

  kgd->tau = scalar_new();
  kgd->psi = zkp_schnorr_new();
  kgd->aux = zkp_aux_info_new(2*sizeof(hash_chunk) + sizeof(uint64_t), NULL); // prepeare for (sid_hash, i, srid)

  kgd->run_time = 0;
}

void cmp_key_generation_clean(cmp_party_t *party)
{
  cmp_key_generation_t *kgd = party->key_generation_data;

  zkp_aux_info_free(kgd->aux);
  zkp_schnorr_free(kgd->psi);
  scalar_free(kgd->tau);
  scalar_free(kgd->secret_x);
  group_elem_free(kgd->public_X);

  free(kgd);
}

void cmp_key_generation_round_1_commit(hash_chunk V, const hash_chunk sid_hash, uint64_t party_id, const cmp_party_t *party)
{
  cmp_key_generation_t *kgd = party->key_generation_data;

  uint8_t *temp_bytes = malloc(GROUP_ELEMENT_BYTES);

  SHA512_CTX sha_ctx;
  SHA512_Init(&sha_ctx);
  SHA512_Update(&sha_ctx, sid_hash, sizeof(hash_chunk));
  SHA512_Update(&sha_ctx, &party_id, sizeof(uint64_t));
  SHA512_Update(&sha_ctx, kgd->srid, sizeof(hash_chunk));
  
  group_elem_to_bytes(&temp_bytes, GROUP_ELEMENT_BYTES, kgd->public_X, party->ec, 0);
  SHA512_Update(&sha_ctx, temp_bytes, GROUP_ELEMENT_BYTES);

  group_elem_to_bytes(&temp_bytes, GROUP_ELEMENT_BYTES, kgd->psi->proof.A, party->ec, 0);
  SHA512_Update(&sha_ctx, temp_bytes, GROUP_ELEMENT_BYTES);
  SHA512_Update(&sha_ctx, kgd->u, sizeof(hash_chunk));
  SHA512_Final(V, &sha_ctx);
  
  free(temp_bytes);
}

void  cmp_key_generation_round_1_exec (cmp_party_t *party)
{
  clock_t time_start = clock();
  uint64_t time_diff;

  cmp_key_generation_t *kgd = party->key_generation_data;

  scalar_sample_in_range(kgd->secret_x, party->ec_order, 0);
  group_operation(kgd->public_X, NULL, party->ec_gen, kgd->secret_x, party->ec);

  kgd->psi->public.G = party->ec;
  kgd->psi->public.g = party->ec_gen;
  zkp_schnorr_commit(kgd->psi, kgd->tau);

  cmp_sample_bytes(kgd->srid, sizeof(hash_chunk));
  cmp_sample_bytes(kgd->u, sizeof(hash_chunk));
  cmp_key_generation_round_1_commit(kgd->V, party->sid, party->id, party);

  time_diff = (clock() - time_start) * 1000 /CLOCKS_PER_SEC;
  kgd->run_time += time_diff;

  printf("### Round 1. Party %lu broadcasts (sid, i, V_i).\t>>>\t%lu B, %lu ms\n", party->id, 2*sizeof(uint64_t) + sizeof(hash_chunk), time_diff);
  
  if (!PRINT_VALUES) return;
  printHexBytes("sid_hash = 0x", party->sid_hash, sizeof(hash_chunk), "\n", 0);
  printf("V_%lu = ", party->index); printHexBytes("0x", kgd->V, sizeof(hash_chunk), "\n", 0);
}

void  cmp_key_generation_round_2_exec (cmp_party_t *party)
{
  clock_t time_start = clock();
  uint64_t time_diff;

  cmp_key_generation_t *kgd = party->key_generation_data;

  // Echo broadcast - Send hash of all V_i commitments
  SHA512_CTX sha_ctx;
  SHA512_Init(&sha_ctx);
  for (uint64_t i = 0; i < party->num_parties; ++i) SHA512_Update(&sha_ctx, party->parties[i]->key_generation_data->V, sizeof(hash_chunk));
  SHA512_Final(kgd->echo_broadcast, &sha_ctx);

  time_diff = (clock() - time_start) * 1000 /CLOCKS_PER_SEC;
  kgd->run_time += time_diff;

  printf("### Round 2. Party %lu publishes (sid, i, srid_i, X_i, A_i, u_i, echo_broadcast_i).\t>>>\t%lu B, %lu ms\n", party->id, 
    2*sizeof(uint64_t) + 4*sizeof(hash_chunk) + 2*GROUP_ELEMENT_BYTES, time_diff);
  
  if (!PRINT_VALUES) return;
  printf("X_%lu = ", party->index); printECPOINT("", kgd->public_X, party->ec, "\n", 1);
  printf("A_%lu = ", party->index); printECPOINT("", kgd->psi->proof.A, party->ec, "\n", 1);
  printf("srid_%lu = ", party->index); printHexBytes("0x", kgd->srid, sizeof(hash_chunk), "\n", 0);
  printf("u_%lu = ", party->index); printHexBytes("0x", kgd->u, sizeof(hash_chunk), "\n", 0);
  printf("echo_broadcast_%lu = ", party->index); printHexBytes("0x", kgd->echo_broadcast, sizeof(hash_chunk), "\n", 0);
}

void  cmp_key_generation_round_3_exec (cmp_party_t *party)
{
  clock_t time_start = clock();
  uint64_t time_diff;

  cmp_key_generation_t *kgd = party->key_generation_data;

  int *verified_decomm = calloc(party->num_parties, sizeof(int));
  int *verified_echo = calloc(party->num_parties, sizeof(int));

  hash_chunk ver_data;
  memcpy(party->srid, kgd->srid, sizeof(hash_chunk));
  for (uint64_t j = 0; j < party->num_parties; ++j)
  {
    if (j == party->index) continue;

    // Verify commited V_i
    cmp_key_generation_round_1_commit(ver_data, party->sid, party->parties_ids[j], party->parties[j]);
    verified_decomm[j] = memcmp(ver_data, party->parties[j]->key_generation_data->V, sizeof(hash_chunk)) == 0;

    // Verify echo broadcast of round 1 commitment -- ToDo: expand to identification of malicious party
    verified_echo[j] = memcmp(kgd->echo_broadcast, party->parties[j]->key_generation_data->echo_broadcast, sizeof(hash_chunk)) == 0;

    // Set srid as xor of all party's srid_i
    for (uint64_t pos = 0; pos < sizeof(hash_chunk); ++pos) party->srid[pos] ^= party->parties[j]->key_generation_data->srid[pos];
  }

  for (uint64_t j = 0; j < party->num_parties; ++j){
    if (j == party->index) continue;
    if (verified_decomm[j] != 1) printf("%sParty %lu: decommitment of V_i from Party %lu\n",ERR_STR, party->id, party->parties_ids[j]);
    if (verified_echo[j] != 1)   printf("%sParty %lu: received different echo broadcast of round 1 from Party %lu\n",ERR_STR, party->id, party->parties_ids[j]);
  }

  free(verified_decomm);
  free(verified_echo);

  // Aux Info (ssid, i, srid)
  uint64_t aux_pos = 0;
  zkp_aux_info_update_move(kgd->aux, &aux_pos, party->sid_hash, sizeof(hash_chunk));
  zkp_aux_info_update_move(kgd->aux, &aux_pos, &party->id, sizeof(uint64_t));
  zkp_aux_info_update_move(kgd->aux, &aux_pos, party->srid, sizeof(hash_chunk));
  assert(kgd->aux->info_len == aux_pos);

  // Set Schnorr ZKP public claim and secret, then prove
  kgd->psi->public.X = kgd->public_X;
  kgd->psi->secret.x = kgd->secret_x;
  zkp_schnorr_prove(kgd->psi, kgd->aux, kgd->tau);

  time_diff = (clock() - time_start) * 1000 /CLOCKS_PER_SEC;
  kgd->run_time += time_diff;
  
  uint64_t psi_bytes;
  zkp_schnorr_proof_to_bytes(NULL, &psi_bytes, NULL, 0);
  printf("### Round 3. Party %lu publishes (sid, i, psi_i).\t>>>\t%lu B, %lu ms\n", party->id, 2*sizeof(uint64_t) + psi_bytes, time_diff);

  if (!PRINT_VALUES) return;
  printHexBytes("combined srid = 0x", party->srid, sizeof(hash_chunk), "\n", 0);
}

void cmp_key_generation_final_exec(cmp_party_t *party)
{
  clock_t time_start = clock();
  uint64_t time_diff;

  cmp_key_generation_t *kgd = party->key_generation_data;

  int *verified_psi = calloc(party->num_parties, sizeof(int));

  // Verify all Schnorr ZKP received from parties  
  for (uint64_t j = 0; j < party->num_parties; ++j)
  {
    if (j == party->index) continue;
    zkp_aux_info_update(kgd->aux, sizeof(hash_chunk), &party->parties_ids[j], sizeof(uint64_t));     // Update i to commiting player
    verified_psi[j] = zkp_schnorr_verify(party->parties[j]->key_generation_data->psi, kgd->aux)
      && group_elem_equal(party->parties[j]->key_generation_data->psi->proof.A, party->parties[j]->key_generation_data->psi->proof.A, party->ec);      // Check A's of rounds 2 and 3
  }

  for (uint64_t j = 0; j < party->num_parties; ++j){
    if (j == party->index) continue;
    if (verified_psi[j] != 1) printf("%sParty %lu: schnorr zkp (psi) failed verification from Party %lu\n",ERR_STR, party->id, party->parties_ids[j]);
  }
  
  free(verified_psi);

  // Set party's values, and update sid_hash to include srid and public_X
  scalar_copy(party->secret_x, kgd->secret_x);
  //scalar_make_plus_minus(party->secret_x, party->ec_order);
  for (uint64_t j = 0; j < party->num_parties; ++j)
  {
    if (!party->public_X[j]) party->public_X[j] = group_elem_new(party->ec);
    group_elem_copy(party->public_X[j], party->parties[j]->key_generation_data->public_X);
  }
  cmp_set_sid_hash(party);
  
  time_diff = (clock() - time_start) * 1000 /CLOCKS_PER_SEC;
  kgd->run_time += time_diff;
  
  printf("### Final. Party %lu stores (srid, all X, secret x_i).\t>>>\t%lu B, %lu ms\n", party->id, 
    sizeof(hash_chunk) + party->num_parties * GROUP_ELEMENT_BYTES + GROUP_ORDER_BYTES, time_diff);

  if (!PRINT_VALUES) return;
  printf("x_%lu = ", party->index); printBIGNUM("", party->secret_x, "\n");
}

/******************************************** 
 * 
 *   Key Refresh and Auxiliary Information 
 * 
 ********************************************/

void cmp_refresh_aux_info_init(cmp_party_t *party)
{
  cmp_refresh_aux_info_t *reda = malloc(sizeof(*reda));
  party->refresh_data = reda;

  reda->paillier_priv = NULL;
  reda->rped_priv = NULL;

  reda->psi_mod  = zkp_paillier_blum_new();
  reda->psi_rped = zkp_ring_pedersen_param_new();
  reda->psi_sch  = calloc(party->num_parties, sizeof(zkp_schnorr_t)); 
  reda->tau      = calloc(party->num_parties, sizeof(scalar_t));
  reda->aux      = zkp_aux_info_new(2*sizeof(hash_chunk) + sizeof(uint64_t), NULL);   // prepare for (sid, i, rho)
  
  reda->reshare_secret_x_j = calloc(party->num_parties, sizeof(scalar_t));
  reda->encrypted_reshare_j = calloc(party->num_parties, sizeof(scalar_t));
  reda->reshare_public_X_j = calloc(party->num_parties, sizeof(gr_elem_t));

  for (uint64_t j = 0; j < party->num_parties; ++j)
  {
    reda->tau[j]                 = scalar_new();
    reda->reshare_secret_x_j[j]  = scalar_new();
    reda->encrypted_reshare_j[j] = scalar_new();
    reda->reshare_public_X_j[j]  = group_elem_new(party->ec);
    reda->psi_sch[j]             = zkp_schnorr_new();
  }

  reda->prime_time = 0;
  reda->run_time = 0;
}

void cmp_refresh_aux_info_clean(cmp_party_t *party)
{
  cmp_refresh_aux_info_t *reda = party->refresh_data;

  zkp_aux_info_free(reda->aux);
  paillier_encryption_free_keys(reda->paillier_priv, NULL);
  ring_pedersen_free_param(reda->rped_priv, NULL);
  zkp_paillier_blum_free(reda->psi_mod);
  zkp_ring_pedersen_param_free(reda->psi_rped);

  for (uint64_t j = 0; j < party->num_parties; ++j)
  {
    scalar_free(reda->tau[j]);
    scalar_free(reda->reshare_secret_x_j[j]);
    scalar_free(reda->encrypted_reshare_j[j]);
    group_elem_free(reda->reshare_public_X_j[j]);
    zkp_schnorr_free(reda->psi_sch[j]);
  }

  free(reda->reshare_secret_x_j);
  free(reda->encrypted_reshare_j);
  free(reda->reshare_public_X_j);
  free(reda->psi_sch);
  free(reda->tau);
  free(reda);
}

void cmp_refresh_aux_info_round_1_commit(hash_chunk V, const hash_chunk sid_hash, uint64_t party_id, const cmp_party_t *party)
{
  cmp_refresh_aux_info_t *reda = party->refresh_data;

  uint8_t *temp_bytes = malloc(PAILLIER_MODULUS_BYTES);     // Enough also for GROUP_ELEMENT_BYTES

  SHA512_CTX sha_ctx;
  SHA512_Init(&sha_ctx);
  SHA512_Update(&sha_ctx, sid_hash, sizeof(hash_chunk));
  SHA512_Update(&sha_ctx, &party_id, sizeof(uint64_t));

  for (uint64_t j = 0; j < party->num_parties; ++j)
  {
    group_elem_to_bytes(&temp_bytes, GROUP_ELEMENT_BYTES, reda->reshare_public_X_j[j], party->ec, 0);
    SHA512_Update(&sha_ctx, temp_bytes, GROUP_ELEMENT_BYTES);
  }

  for (uint64_t j = 0; j < party->num_parties; ++j)
  {
    group_elem_to_bytes(&temp_bytes, GROUP_ELEMENT_BYTES, reda->psi_sch[j]->proof.A, party->ec, 0);
    SHA512_Update(&sha_ctx, temp_bytes, GROUP_ELEMENT_BYTES);
  }

  scalar_to_bytes(&temp_bytes, PAILLIER_MODULUS_BYTES, reda->paillier_priv->pub.N, 0);
  SHA512_Update(&sha_ctx, temp_bytes, GROUP_ELEMENT_BYTES);

  scalar_to_bytes(&temp_bytes, PAILLIER_MODULUS_BYTES, reda->rped_priv->pub.N, 0);
  SHA512_Update(&sha_ctx, temp_bytes, GROUP_ELEMENT_BYTES);

  scalar_to_bytes(&temp_bytes, PAILLIER_MODULUS_BYTES, reda->rped_priv->pub.s, 0);
  SHA512_Update(&sha_ctx, temp_bytes, GROUP_ELEMENT_BYTES);

  scalar_to_bytes(&temp_bytes, PAILLIER_MODULUS_BYTES, reda->rped_priv->pub.t, 0);
  SHA512_Update(&sha_ctx, temp_bytes, GROUP_ELEMENT_BYTES);
  SHA512_Update(&sha_ctx, reda->rho, sizeof(hash_chunk));
  SHA512_Update(&sha_ctx, reda->u, sizeof(hash_chunk));
  SHA512_Final(V, &sha_ctx);
  
  free(temp_bytes);
}

void cmp_refresh_aux_info_round_1_exec (cmp_party_t *party)
{
  clock_t time_start = clock();
  uint64_t time_diff;

  cmp_refresh_aux_info_t *reda = party->refresh_data;

  reda->paillier_priv = paillier_encryption_generate_key(4*PAILLIER_MODULUS_BYTES);
  reda->rped_priv = ring_pedersen_generate_param(4*RING_PED_MODULUS_BYTES);
  
  time_diff = (clock() - time_start) * 1000 /CLOCKS_PER_SEC;
  reda->prime_time = time_diff;

  time_start = clock();
  
  // Sample other parties' reshares, set negative of sum for current
  scalar_set_ul(reda->reshare_secret_x_j[party->index], 0);
  for (uint64_t j = 0; j < party->num_parties; ++j)
  {
    // Also initialize relevant zkp
    reda->psi_sch[j]->public.G = party->ec;
    reda->psi_sch[j]->public.g = party->ec_gen;
    zkp_schnorr_commit(reda->psi_sch[j], reda->tau[j]);

    // Dont choose your own values, only later if needed
    if (j == party->index) continue; 

    scalar_sample_in_range(reda->reshare_secret_x_j[j], party->ec_order, 0);
    group_operation(reda->reshare_public_X_j[j], NULL, party->ec_gen, reda->reshare_secret_x_j[j], party->ec);
    scalar_sub(reda->reshare_secret_x_j[party->index], reda->reshare_secret_x_j[party->index], reda->reshare_secret_x_j[j], party->ec_order);
  }
  group_operation(reda->reshare_public_X_j[party->index], NULL, party->ec_gen, reda->reshare_secret_x_j[party->index], party->ec);

  cmp_sample_bytes(reda->rho, sizeof(hash_chunk));
  cmp_sample_bytes(reda->u, sizeof(hash_chunk));
  cmp_refresh_aux_info_round_1_commit(reda->V, party->sid, party->id, party);

  time_diff = (clock() - time_start) * 1000 /CLOCKS_PER_SEC;
  reda->run_time += time_diff;

  printf("### Round 1. Party %lu broadcasts (sid, i, V_i).\t>>>\t%lu B, %lu ms (gen N_i) + %lu ms (rest)\n", party->id, 2*sizeof(hash_chunk)+sizeof(uint64_t), reda->prime_time, time_diff);

  if (!PRINT_VALUES) return;
  printHexBytes("sid_hash = 0x", party->sid_hash, sizeof(hash_chunk), "\n", 0);
  printf("V_%lu = ", party->index); printHexBytes("0x", reda->V, sizeof(hash_chunk), "\n", 0);
  for (uint64_t j = 0; j < party->num_parties; ++j)
  {
    printf("x_%lue%lu = ", party->index, j); printBIGNUM("", reda->reshare_secret_x_j[j], "\n");
  }
}

void  cmp_refresh_aux_info_round_2_exec (cmp_party_t *party)
{
  clock_t time_start = clock();
  uint64_t time_diff;

  cmp_refresh_aux_info_t *reda = party->refresh_data;

  // Echo broadcast - Send hash of all V_i commitments
  SHA512_CTX sha_ctx;
  SHA512_Init(&sha_ctx);
  for (uint64_t i = 0; i < party->num_parties; ++i) SHA512_Update(&sha_ctx, party->parties[i]->refresh_data->V, sizeof(hash_chunk));
  SHA512_Final(reda->echo_broadcast, &sha_ctx);

  time_diff = (clock() - time_start) * 1000 /CLOCKS_PER_SEC;
  reda->run_time += time_diff;

  printf("### Round 2. Party %lu publishes (sid, i, X_i^{1...n}, A_i^{1...n}, Paillier N_i, s_i, t_i, rho_i, u_i, echo_broadcast).\t>>>\t%lu B, %lu ms\n", 
    party->id, 2*sizeof(uint64_t) + party->num_parties*2*GROUP_ELEMENT_BYTES + 3*PAILLIER_MODULUS_BYTES + 3*sizeof(hash_chunk), time_diff);
  
  if (!PRINT_VALUES) return;
  printf("echo_broadcast_%lu = ", party->index); printHexBytes("echo_broadcast = 0x", reda->echo_broadcast, sizeof(hash_chunk), "\n", 0);
  printf("rho_%lu = ", party->index); printHexBytes("0x", reda->rho, sizeof(hash_chunk), "\n", 0);
  printf("u_%lu = ", party->index); printHexBytes("0x", reda->u, sizeof(hash_chunk), "\n", 0);
  printf("N_%lu = ", party->index); printBIGNUM("", reda->rped_priv->pub.N, "\n");
  printf("s_%lu = ", party->index); printBIGNUM("", reda->rped_priv->pub.s, "\n");
  printf("t_%lu = ", party->index); printBIGNUM("", reda->rped_priv->pub.t, "\n");

  for (uint64_t j = 0; j < party->num_parties; ++j)
  {
    printf("X_%lue%lu = ", party->index, j); printECPOINT("", reda->reshare_public_X_j[j], party->ec, "\n", 1);
    printf("A_%lue%lu = ", party->index, j); printECPOINT("", reda->psi_sch[j]->proof.A, party->ec, "\n", 1);
  }
}

void  cmp_refresh_aux_info_round_3_exec (cmp_party_t *party)
{
  clock_t time_start = clock();
  uint64_t time_diff;

  cmp_refresh_aux_info_t *reda = party->refresh_data;
  
  gr_elem_t combined_public = group_elem_new(party->ec);

  int *verified_modulus_size = calloc(party->num_parties, sizeof(int));
  int *verified_public_shares = calloc(party->num_parties, sizeof(int));
  int *verified_decomm = calloc(party->num_parties, sizeof(int));
  int *verified_echo = calloc(party->num_parties, sizeof(int));

  hash_chunk ver_data;
  memcpy(reda->combined_rho, reda->rho, sizeof(hash_chunk));
  for (uint64_t j = 0; j < party->num_parties; ++j)
  {
    if (j == party->index) continue; 

    // Verify modulus size
    verified_modulus_size[j] = scalar_bitlength(party->parties[j]->refresh_data->paillier_priv->pub.N) >= 8*PAILLIER_MODULUS_BYTES-1;

    // Verify shared public X_j is valid
    group_operation(combined_public, NULL, NULL, NULL, party->ec);
    for (uint64_t k = 0; k < party->num_parties; ++k) {
      group_operation(combined_public, combined_public, party->parties[j]->refresh_data->reshare_public_X_j[k], NULL, party->ec);
    }
    verified_public_shares[j] = group_elem_is_ident(combined_public, party->ec) == 1;

    // Verify commited V_i
    cmp_refresh_aux_info_round_1_commit(ver_data, party->sid, party->parties_ids[j], party->parties[j]);
    verified_decomm[j] = memcmp(ver_data, party->parties[j]->refresh_data->V, sizeof(hash_chunk)) == 0;

    // Verify echo broadcast of round 1 commitment -- ToDo: expand to identification of malicious party
    verified_echo[j] = memcmp(reda->echo_broadcast, party->parties[j]->refresh_data->echo_broadcast, sizeof(hash_chunk)) == 0;

    // Set combined rho as xor of all party's rho_i
    for (uint64_t pos = 0; pos < sizeof(hash_chunk); ++pos) reda->combined_rho[pos] ^= party->parties[j]->refresh_data->rho[pos];


    if (verified_modulus_size[j] != 1) printf("%sParty %lu: N_i bitlength from Party %lu\n",ERR_STR, party->id, party->parties_ids[j]);
    if (verified_public_shares[j] != 1) printf("%sParty %lu: invalid X_j_k sharing from Party %lu\n",ERR_STR, party->id, party->parties_ids[j]);
    if (verified_decomm[j] != 1)        printf("%sParty %lu: decommitment of V_i from Party %lu\n",ERR_STR, party->id, party->parties_ids[j]);
    if (verified_echo[j] != 1)          printf("%sParty %lu: received different echo broadcast of round 1 from Party %lu\n",ERR_STR, party->id, party->parties_ids[j]);
  }

  free(verified_modulus_size);
  free(verified_public_shares);
  free(verified_decomm);
  free(verified_echo);
  group_elem_free(combined_public);

    // Aux Info for ZKP (ssid, i, combined rho)
  uint64_t aux_pos = 0;
  zkp_aux_info_update_move(reda->aux, &aux_pos, party->sid_hash, sizeof(hash_chunk));
  zkp_aux_info_update_move(reda->aux, &aux_pos, &party->id, sizeof(uint64_t));
  zkp_aux_info_update_move(reda->aux, &aux_pos, reda->combined_rho, sizeof(hash_chunk));
  assert(reda->aux->info_len == aux_pos);

  // Generate ZKP, set public claim and secret, then prove
  reda->psi_mod->public = &reda->paillier_priv->pub;
  reda->psi_mod->private = reda->paillier_priv;
  zkp_paillier_blum_prove(reda->psi_mod, reda->aux);

  reda->psi_rped->rped_pub = &reda->rped_priv->pub;
  reda->psi_rped->secret = reda->rped_priv;
  zkp_ring_pedersen_param_prove(reda->psi_rped, reda->aux);

  scalar_t temp_paillier_rand = scalar_new();
  for (uint64_t j = 0; j < party->num_parties; ++j)
  {
    // Encrypt all secret reshares (including own) - ToDo add echo broadcast on these
    paillier_encryption_sample(temp_paillier_rand, &party->parties[j]->refresh_data->paillier_priv->pub);
    paillier_encryption_encrypt(reda->encrypted_reshare_j[j], reda->reshare_secret_x_j[j], temp_paillier_rand, &party->parties[j]->refresh_data->paillier_priv->pub);

    reda->psi_sch[j]->public.X = reda->reshare_public_X_j[j];
    reda->psi_sch[j]->secret.x = reda->reshare_secret_x_j[j];
    zkp_schnorr_prove(reda->psi_sch[j], reda->aux, reda->tau[j]);
  }
  scalar_free(temp_paillier_rand);

  time_diff = (clock() - time_start) * 1000 /CLOCKS_PER_SEC;
  reda->run_time += time_diff;
  
  uint64_t psi_sch_bytes;
  uint64_t psi_rped_bytes;
  uint64_t psi_mod_bytes;
  zkp_schnorr_proof_to_bytes(NULL, &psi_sch_bytes, NULL, 0);
  zkp_ring_pedersen_param_proof_to_bytes(NULL, &psi_rped_bytes, NULL, 0);
  zkp_paillier_blum_proof_to_bytes(NULL, &psi_mod_bytes, NULL, 0);


  printf("### Round 3. Party %lu publishes (sid, i, psi_mod, psi_rped, psi_sch^j, Enc_j(x_i^j)).\t>>>\t%lu B, %lu ms\n", party->id, 
    2*sizeof(uint64_t) + psi_mod_bytes + psi_rped_bytes + (party->num_parties-1)*psi_sch_bytes + party->num_parties*2*PAILLIER_MODULUS_BYTES, time_diff);

  if (!PRINT_VALUES) return;
  printHexBytes("combined rho = 0x", reda->combined_rho, sizeof(hash_chunk), "\n", 0);
}

void cmp_refresh_aux_info_final_exec(cmp_party_t *party)
{
  clock_t time_start = clock();
  uint64_t time_diff;

  cmp_refresh_aux_info_t *reda = party->refresh_data;

  int *verified_reshare  = calloc(party->num_parties, sizeof(int));
  int *verified_psi_mod  = calloc(party->num_parties, sizeof(int));
  int *verified_psi_rped = calloc(party->num_parties, sizeof(int));
  int *verified_psi_sch  = calloc(party->num_parties*party->num_parties, sizeof(int));

  // Verify all Schnorr ZKP and values received from parties  

  scalar_t received_reshare = scalar_new();
  scalar_t sum_received_reshares = scalar_new();
  gr_elem_t ver_public = group_elem_new(party->ec);
  
  for (uint64_t j = 0; j < party->num_parties; ++j)
  { 
    // Decrypt and verify reshare secret vs public   
    paillier_encryption_decrypt(received_reshare, party->parties[j]->refresh_data->encrypted_reshare_j[party->index], reda->paillier_priv);     // TODO: reduce MODULU q!!!
    scalar_add(sum_received_reshares, sum_received_reshares, received_reshare, party->ec_order);
    group_operation(ver_public, NULL, party->ec_gen, received_reshare, party->ec);
    verified_reshare[j] = group_elem_equal(ver_public, party->parties[j]->refresh_data->reshare_public_X_j[party->index], party->ec) == 1;

    if (j == party->index) continue; 

    zkp_aux_info_update(reda->aux, sizeof(hash_chunk), &party->parties_ids[j], sizeof(uint64_t));                  // Update i to commiting player
    verified_psi_mod[j] = zkp_paillier_blum_verify(party->parties[j]->refresh_data->psi_mod, reda->aux) == 1;
    verified_psi_rped[j] = zkp_ring_pedersen_param_verify(party->parties[j]->refresh_data->psi_rped, reda->aux) == 1;

    for (uint64_t k = 0; k < party->num_parties; ++k)
    {
      verified_psi_sch[k + party->num_parties*j] = (zkp_schnorr_verify(party->parties[j]->refresh_data->psi_sch[k], reda->aux) == 1)
       && (group_elem_equal(party->parties[j]->refresh_data->psi_sch[k]->proof.A, party->parties[j]->refresh_data->psi_sch[k]->proof.A, party->ec) == 1);      // Check A's of rounds 2 and 3
    }
  }
  scalar_free(received_reshare);

  for (uint64_t j = 0; j < party->num_parties; ++j)
  {
    if (j == party->index) continue; 

    if (verified_reshare[j] != 1)  printf("%sParty %lu: Public reshare inconsistent from Party %lu\n",ERR_STR, party->id, party->parties_ids[j]);
    if (verified_psi_mod[j] != 1)  printf("%sParty %lu: Paillier-Blum ZKP failed verification from Party %lu\n",ERR_STR, party->id, party->parties_ids[j]);
    if (verified_psi_rped[j] != 1) printf("%sParty %lu: Ring-Pedersen ZKP failed verification from Party %lu\n",ERR_STR, party->id, party->parties_ids[j]);
    for (uint64_t k = 0; k < party->num_parties; ++k)
    {
      if (verified_psi_sch[k + party->num_parties*j] != 1) printf("%sParty %lu: Schnorr ZKP failed verification from Party %lu for Party %lu\n",ERR_STR, party->id, party->parties_ids[j], party->parties_ids[k]);
    }
  }

  free(verified_reshare);
  free(verified_psi_mod);
  free(verified_psi_rped);
  free(verified_psi_sch);
  group_elem_free(ver_public);

  // Refresh Party's values
  if (party->paillier_priv) paillier_encryption_free_keys(party->paillier_priv, NULL);
  party->paillier_priv = paillier_encryption_duplicate_key(reda->paillier_priv);

  cmp_set_sid_hash(party);

  scalar_add(party->secret_x, party->secret_x, sum_received_reshares, party->ec_order);
  scalar_free(sum_received_reshares);

  for (uint64_t i = 0; i < party->num_parties; ++i)
  {
    for (uint64_t k = 0; k < party->num_parties; ++k) group_operation(party->public_X[i], party->public_X[i], party->parties[k]->refresh_data->reshare_public_X_j[i], NULL, party->ec);

    party->paillier_pub[i] = paillier_encryption_copy_public(party->parties[i]->refresh_data->paillier_priv);
    party->rped_pub[i] = ring_pedersen_copy_public(party->parties[i]->refresh_data->rped_priv);
  }

  // UDIBUG: Sanity Check
  gr_elem_t check_my_public = group_elem_new(party->ec);
  group_operation(check_my_public, NULL, party->ec_gen, party->secret_x, party->ec);
  assert( group_elem_equal(check_my_public, party->public_X[party->index], party->ec) );
  group_elem_free(check_my_public);

  time_diff = (clock() - time_start) * 1000 /CLOCKS_PER_SEC;
  reda->run_time += time_diff;
  
  printf("### Final. Party %lu stores fresh (secret x_i, all public X, N_i, s_i, t_i).\t>>>\t%lu B, %lu ms\n", party->id, 
    party->num_parties * GROUP_ELEMENT_BYTES + GROUP_ORDER_BYTES + party->num_parties*3*PAILLIER_MODULUS_BYTES, time_diff);

  if (!PRINT_VALUES) return;
  printf("fresh_x_%lu = ", party->index); printBIGNUM("", party->secret_x, "\n");
  for (uint64_t i = 0; i < party->num_parties; ++i) 
  {
    printf("X_%lu = ", i); printECPOINT("", party->public_X[i], party->ec, "\n", 1);
    printf("N_%lu = ", i); printBIGNUM("", party->rped_pub[i]->N, "\n");
    printf("s_%lu = ", i); printBIGNUM("", party->rped_pub[i]->s, "\n");
    printf("t_%lu = ", i); printBIGNUM("", party->rped_pub[i]->t, "\n");
  }
}

/******************************************** 
 * 
 *   Pre-Signing
 * 
 ********************************************/

void cmp_presigning_init(cmp_party_t *party)
{
  cmp_presigning_t *preda = malloc(sizeof(*preda));
  party->presigning_data = preda;

  preda->G     = scalar_new();
  preda->K     = scalar_new();
  preda->k     = scalar_new();
  preda->gamma = scalar_new();
  preda->rho   = scalar_new();
  preda->nu    = scalar_new();
  preda->delta = scalar_new();
  preda->chi   = scalar_new();

  preda->Delta          = group_elem_new(party->ec);
  preda->Gamma          = group_elem_new(party->ec);
  preda->combined_Gamma = group_elem_new(party->ec);

  preda->alpha_j    = calloc(party->num_parties, sizeof(scalar_t));
  preda->beta_j     = calloc(party->num_parties, sizeof(scalar_t));
  preda->alphahat_j = calloc(party->num_parties, sizeof(scalar_t));
  preda->betahat_j  = calloc(party->num_parties, sizeof(scalar_t));
  preda->D_j        = calloc(party->num_parties, sizeof(scalar_t));
  preda->F_j        = calloc(party->num_parties, sizeof(scalar_t));
  preda->Dhat_j     = calloc(party->num_parties, sizeof(scalar_t));
  preda->Fhat_j     = calloc(party->num_parties, sizeof(scalar_t));

  preda->psi_enc  = calloc(party->num_parties, sizeof(zkp_encryption_in_range_t));
  preda->psi_affp = calloc(party->num_parties, sizeof(zkp_operation_paillier_commitment_range_t));
  preda->psi_affg = calloc(party->num_parties, sizeof(zkp_operation_group_commitment_range_t));
  preda->psi_logG = calloc(party->num_parties, sizeof(zkp_group_vs_paillier_range_t));
  preda->psi_logK = calloc(party->num_parties, sizeof(zkp_group_vs_paillier_range_t));

  preda->aux      = zkp_aux_info_new(sizeof(hash_chunk) + sizeof(uint64_t), NULL);      // Prepate for (sid_hash, i);
  
  for (uint64_t j = 0; j < party->num_parties; ++j){
    preda->alpha_j[j]    = scalar_new();
    preda->beta_j[j]     = scalar_new();
    preda->alphahat_j[j] = scalar_new();
    preda->betahat_j[j]  = scalar_new();
    preda->D_j[j]        = scalar_new();
    preda->F_j[j]        = scalar_new();
    preda->Dhat_j[j]     = scalar_new();
    preda->Fhat_j[j]     = scalar_new();

    if (j == party->index) continue;

    preda->psi_enc [j] = zkp_encryption_in_range_new();
    preda->psi_affp[j] = zkp_operation_paillier_commitment_range_new();
    preda->psi_affg[j] = zkp_operation_group_commitment_range_new();
    preda->psi_logG[j] = zkp_group_vs_paillier_range_new();
    preda->psi_logK[j] = zkp_group_vs_paillier_range_new();
  }

  preda->run_time = 0;
}

void cmp_presigning_clean(cmp_party_t *party)
{
  cmp_presigning_t *preda = party->presigning_data;

  scalar_free(preda->G    );
  scalar_free(preda->K    );
  scalar_free(preda->k    );
  scalar_free(preda->gamma);
  scalar_free(preda->rho  );
  scalar_free(preda->nu   );
  scalar_free(preda->delta);
  scalar_free(preda->chi);

  group_elem_free(preda->Delta         );
  group_elem_free(preda->Gamma         );
  group_elem_free(preda->combined_Gamma);

  for (uint64_t j = 0; j < party->num_parties; ++j)
  {
    scalar_free(preda->alpha_j[j]);
    scalar_free(preda->beta_j[j] );
    scalar_free(preda->alphahat_j[j]);
    scalar_free(preda->betahat_j[j] );
    scalar_free(preda->D_j[j]    );
    scalar_free(preda->F_j[j]    );
    scalar_free(preda->Dhat_j[j] );
    scalar_free(preda->Fhat_j[j] );

    if (j == party->index) continue;

    zkp_encryption_in_range_free(preda->psi_enc [j]);
    zkp_operation_paillier_commitment_range_free(preda->psi_affp[j]);
    zkp_operation_group_commitment_range_free(preda->psi_affg[j]);
    zkp_group_vs_paillier_range_free(preda->psi_logG [j]);
    zkp_group_vs_paillier_range_free(preda->psi_logK [j]);
  }

  zkp_aux_info_free(preda->aux);

  free(preda->alphahat_j);
  free(preda->betahat_j );
  free(preda->alpha_j);
  free(preda->beta_j );
  free(preda->D_j    );
  free(preda->F_j    );
  free(preda->Dhat_j );
  free(preda->Fhat_j );

  free(preda->psi_enc );
  free(preda->psi_affp);
  free(preda->psi_affg);
  free(preda->psi_logG);
  free(preda->psi_logK);
  free(preda);
}


void cmp_presigning_round_1_exec (cmp_party_t *party)
{
  clock_t time_start = clock();
  uint64_t time_diff;

  cmp_presigning_t *preda = party->presigning_data;

  paillier_encryption_sample(preda->rho, &party->paillier_priv->pub);
  scalar_sample_in_range(preda->k, party->ec_order, 0);
  paillier_encryption_encrypt(preda->K, preda->k, preda->rho, &party->paillier_priv->pub);

  paillier_encryption_sample(preda->nu, &party->paillier_priv->pub);
  scalar_sample_in_range(preda->gamma, party->ec_order, 0);
  paillier_encryption_encrypt(preda->G, preda->gamma, preda->nu, &party->paillier_priv->pub);

  uint64_t aux_pos = 0;
  zkp_aux_info_update_move(preda->aux, &aux_pos, party->sid_hash, sizeof(hash_chunk));
  zkp_aux_info_update_move(preda->aux, &aux_pos, &party->id, sizeof(uint64_t));
  assert(preda->aux->info_len == aux_pos);

  for (uint64_t j = 0; j < party->num_parties; ++j) 
  {
    if (j == party->index) continue;

    preda->psi_enc[j]->public.paillier_pub = &party->paillier_priv->pub;
    preda->psi_enc[j]->public.rped_pub = party->rped_pub[j];
    preda->psi_enc[j]->public.G = party->ec;
    preda->psi_enc[j]->public.K = preda->K;
    preda->psi_enc[j]->public.k_range_bytes = CALIGRAPHIC_I_ZKP_RANGE_BYTES;
    preda->psi_enc[j]->secret.k = preda->k;
    preda->psi_enc[j]->secret.rho = preda->rho;
    zkp_encryption_in_range_prove(preda->psi_enc[j], preda->aux);
  }
  time_diff = (clock() - time_start) * 1000 /CLOCKS_PER_SEC;
  preda->run_time += time_diff;

  uint64_t psi_enc_bytes;
  zkp_encryption_in_range_proof_to_bytes(NULL, &psi_enc_bytes, NULL, CALIGRAPHIC_I_ZKP_RANGE_BYTES, 0);
  printf("### Round 1. Party %lu broadcasts (sid, i, K_i, G_i). Send (sid, i, psi_enc_j) to each Party j.\t>>>\t%lu B, %lu ms\n", party->id,
    2*sizeof(hash_chunk) + sizeof(uint64_t) + 4*PAILLIER_MODULUS_BYTES + (party->num_parties-1) * psi_enc_bytes, time_diff);

  if (!PRINT_VALUES) return;
  printHexBytes("sid_hash = 0x", party->sid_hash, sizeof(hash_chunk), "\n", 0);
  printf("k_%lu = ", party->index); printBIGNUM("", preda->k, "\n");
  printf("gamma_%lu = ", party->index); printBIGNUM("", preda->gamma, "\n");
}

void  cmp_presigning_round_2_exec (cmp_party_t *party)
{
  clock_t time_start = clock();
  uint64_t time_diff;

  cmp_presigning_t *preda = party->presigning_data;

  int *verified_psi_enc = calloc(party->num_parties, sizeof(int));

  // Echo broadcast - Send hash of all K_j,G_j
  uint8_t *temp_bytes = malloc(PAILLIER_MODULUS_BYTES);

  SHA512_CTX sha_ctx;
  SHA512_Init(&sha_ctx);
  for (uint64_t i = 0; i < party->num_parties; ++i)
  {
    scalar_to_bytes(&temp_bytes, PAILLIER_MODULUS_BYTES, party->parties[i]->presigning_data->K, 0);
    SHA512_Update(&sha_ctx, temp_bytes, PAILLIER_MODULUS_BYTES);
    scalar_to_bytes(&temp_bytes, PAILLIER_MODULUS_BYTES, party->parties[i]->presigning_data->G, 0);
    SHA512_Update(&sha_ctx, temp_bytes, PAILLIER_MODULUS_BYTES);
  }
  SHA512_Final(preda->echo_broadcast, &sha_ctx);
  free(temp_bytes);

  // Verify psi_enc received
  for (uint64_t j = 0; j < party->num_parties; ++j)
  {
    if (j == party->index) continue;

    zkp_aux_info_update(preda->aux, sizeof(hash_chunk), &party->parties_ids[j], sizeof(uint64_t));
    verified_psi_enc[j] = zkp_encryption_in_range_verify(party->parties[j]->presigning_data->psi_enc[party->index], preda->aux);
    if (verified_psi_enc[j] != 1)  printf("%sParty %lu: failed verification of psi_enc from Party %lu\n",ERR_STR, party->id, party->parties_ids[j]);
  }
  free(verified_psi_enc);

  zkp_aux_info_update(preda->aux, sizeof(hash_chunk), &party->id, sizeof(uint64_t));

  group_operation(preda->Gamma, NULL, party->ec_gen, preda->gamma, party->ec);

  // Executing MtA with relevant ZKP

  scalar_t r          = scalar_new();
  scalar_t s          = scalar_new();
  scalar_t temp_enc   = scalar_new();
  scalar_t beta_range = scalar_new();

  scalar_set_power_of_2(beta_range, 8*CALIGRAPHIC_J_ZKP_RANGE_BYTES);
  for (uint64_t j = 0; j < party->num_parties; ++j)
  {
    if (j == party->index) continue;
    
    // Create ZKP Paillier homomorphic operation against Paillier commitment

    scalar_sample_in_range(preda->beta_j[j], beta_range, 0);
    scalar_make_plus_minus(preda->beta_j[j], beta_range);
    paillier_encryption_sample(r, &party->paillier_priv->pub);
    paillier_encryption_encrypt(preda->F_j[j], preda->beta_j[j], r, &party->paillier_priv->pub);

    // ARTICLE-MOD: using \beta (and not -\beta) for both F and affine operation (later will compute \alpha-\beta in summation)
    paillier_encryption_sample(s, party->paillier_pub[j]);
    paillier_encryption_encrypt(temp_enc, preda->beta_j[j], s, party->paillier_pub[j]);
    paillier_encryption_homomorphic(preda->D_j[j], party->parties[j]->presigning_data->K, preda->gamma, temp_enc, party->paillier_pub[j]);

    preda->psi_affp[j]->public.paillier_pub_0 = party->paillier_pub[j];
    preda->psi_affp[j]->public.paillier_pub_1 = &party->paillier_priv->pub;
    preda->psi_affp[j]->public.rped_pub = party->rped_pub[j];
    preda->psi_affp[j]->public.C = party->parties[j]->presigning_data->K;
    preda->psi_affp[j]->public.G = party->ec;
    preda->psi_affp[j]->public.D = preda->D_j[j];
    preda->psi_affp[j]->public.X = preda->G;
    preda->psi_affp[j]->public.Y = preda->F_j[j];
    preda->psi_affp[j]->public.x_range_bytes = CALIGRAPHIC_I_ZKP_RANGE_BYTES;
    preda->psi_affp[j]->public.y_range_bytes = CALIGRAPHIC_J_ZKP_RANGE_BYTES;
    preda->psi_affp[j]->secret.x = preda->gamma;
    preda->psi_affp[j]->secret.y = preda->beta_j[j];
    preda->psi_affp[j]->secret.rho_x = preda->nu;
    preda->psi_affp[j]->secret.rho_y = r;
    preda->psi_affp[j]->secret.rho = s;
    zkp_operation_paillier_commitment_range_prove(preda->psi_affp[j], preda->aux);

    // Create ZKP Paillier homomorphic operation against Group commitment

    scalar_sample_in_range(preda->betahat_j[j], beta_range, 0);
    scalar_make_plus_minus(preda->betahat_j[j], beta_range);
    paillier_encryption_sample(r, &party->paillier_priv->pub);
    paillier_encryption_encrypt(preda->Fhat_j[j], preda->betahat_j[j], r, &party->paillier_priv->pub);

    // ARTICLE-MOD: using \betahat (and not -\betahat) for both F and affine operation (later will compute \alphahat-\betahat in summation)
    paillier_encryption_sample(s, party->paillier_pub[j]);
    paillier_encryption_encrypt(temp_enc, preda->betahat_j[j], s, party->paillier_pub[j]);
    paillier_encryption_homomorphic(preda->Dhat_j[j], party->parties[j]->presigning_data->K, party->secret_x, temp_enc, party->paillier_pub[j]);

    preda->psi_affg[j]->public.paillier_pub_0 = party->paillier_pub[j];
    preda->psi_affg[j]->public.paillier_pub_1 = &party->paillier_priv->pub;
    preda->psi_affg[j]->public.rped_pub = party->rped_pub[j];
    preda->psi_affg[j]->public.G = party->ec;
    preda->psi_affg[j]->public.g = party->ec_gen;
    preda->psi_affg[j]->public.C = party->parties[j]->presigning_data->K;
    preda->psi_affg[j]->public.D = preda->Dhat_j[j];
    preda->psi_affg[j]->public.X = party->public_X[party->index];
    preda->psi_affg[j]->public.Y = preda->Fhat_j[j];
    preda->psi_affg[j]->public.x_range_bytes = CALIGRAPHIC_I_ZKP_RANGE_BYTES;
    preda->psi_affg[j]->public.y_range_bytes = CALIGRAPHIC_J_ZKP_RANGE_BYTES;
    preda->psi_affg[j]->secret.x = party->secret_x;
    preda->psi_affg[j]->secret.y = preda->betahat_j[j];
    preda->psi_affg[j]->secret.rho_y = r;
    preda->psi_affg[j]->secret.rho = s;
    zkp_operation_group_commitment_range_prove(preda->psi_affg[j], preda->aux);

    // Create group vs Paillier in range ZKP

    preda->psi_logG[j]->public.paillier_pub = &party->paillier_priv->pub;
    preda->psi_logG[j]->public.rped_pub = party->rped_pub[j];
    preda->psi_logG[j]->public.G = party->ec;
    preda->psi_logG[j]->public.g = party->ec_gen;
    preda->psi_logG[j]->public.X = preda->Gamma;
    preda->psi_logG[j]->public.C = preda->G;
    preda->psi_logG[j]->public.x_range_bytes = CALIGRAPHIC_I_ZKP_RANGE_BYTES;
    preda->psi_logG[j]->secret.x = preda->gamma;
    preda->psi_logG[j]->secret.rho = preda->nu;
    zkp_group_vs_paillier_range_prove(preda->psi_logG[j], preda->aux);
  }

  scalar_free(beta_range);
  scalar_free(temp_enc);
  scalar_free(r);
  scalar_free(s);

  time_diff = (clock() - time_start) * 1000 /CLOCKS_PER_SEC;
  preda->run_time += time_diff;

  uint64_t psi_affp_bytes;
  uint64_t psi_affg_bytes;
  uint64_t psi_logG_bytes;
  zkp_operation_paillier_commitment_range_proof_to_bytes(NULL, &psi_affp_bytes, NULL, CALIGRAPHIC_I_ZKP_RANGE_BYTES, CALIGRAPHIC_J_ZKP_RANGE_BYTES, 0);
  zkp_operation_group_commitment_range_proof_to_bytes(NULL, &psi_affg_bytes, NULL, CALIGRAPHIC_I_ZKP_RANGE_BYTES, CALIGRAPHIC_J_ZKP_RANGE_BYTES, 0);
  zkp_group_vs_paillier_range_proof_to_bytes(NULL, &psi_logG_bytes, NULL, CALIGRAPHIC_I_ZKP_RANGE_BYTES, 0);

  printf("### Round 2. Party %lu sends (sid, i, Gamma_i, D_{j,i}, F_{j,i}, D^_{j,i}, F^_{j,i}, psi_affp_j, psi_affg_j, psi_logG_j) to each Party j.\t>>>\t%lu B, %lu ms\n", party->id, 2*sizeof(uint64_t) + GROUP_ELEMENT_BYTES + (party->num_parties-1) * ( 4*PAILLIER_MODULUS_BYTES + psi_affp_bytes + psi_affg_bytes + psi_logG_bytes), time_diff);
  
  if (!PRINT_VALUES) return;
  printf("Gamma_%lu = ", party->index); printECPOINT("", preda->Gamma, party->ec, "\n", 1);
  for (uint64_t j = 0; j < party->num_parties; ++j)
  {
    if (j == party->index) continue;

    printf("beta_%lue%lu = ", party->index, j); printBIGNUM("", preda->beta_j[j], "\n");
    printf("betahat_%lue%lu = ", party->index, j); printBIGNUM("", preda->betahat_j[j],  "\n");
  }
}

void  cmp_presigning_round_3_exec (cmp_party_t *party)
{
  clock_t time_start = clock();
  uint64_t time_diff;

  cmp_presigning_t *preda = party->presigning_data;
  
  // Verify ZKP

  int *verified_psi_affp = calloc(party->num_parties, sizeof(int));
  int *verified_psi_affg = calloc(party->num_parties, sizeof(int));
  int *verified_psi_logG = calloc(party->num_parties, sizeof(int));

  for (uint64_t j = 0; j < party->num_parties; ++j)
  {
    if (j == party->index) continue; 

    zkp_aux_info_update(preda->aux, sizeof(hash_chunk), &party->parties_ids[j], sizeof(uint64_t));

    verified_psi_affp[j] = zkp_operation_paillier_commitment_range_verify(party->parties[j]->presigning_data->psi_affp[party->index], preda->aux);
    verified_psi_affg[j] = zkp_operation_group_commitment_range_verify(party->parties[j]->presigning_data->psi_affg[party->index], preda->aux);
    verified_psi_logG[j] = zkp_group_vs_paillier_range_verify(party->parties[j]->presigning_data->psi_logG[party->index], preda->aux);

    if (verified_psi_affp[j] != 1) printf("%sParty %lu: failed verification of psi_affp from Party %lu\n",ERR_STR, party->id, party->parties_ids[j]);
    if (verified_psi_affg[j] != 1) printf("%sParty %lu: failed verification of psi_affg from Party %lu\n",ERR_STR, party->id, party->parties_ids[j]);
    if (verified_psi_logG[j] != 1) printf("%sParty %lu: failed verification of psi_logG from Party %lu\n",ERR_STR, party->id, party->parties_ids[j]);
  }
  free(verified_psi_affp);
  free(verified_psi_affg);
  free(verified_psi_logG);

  group_operation(preda->combined_Gamma, NULL, NULL, NULL, party->ec);
  for (uint64_t i = 0; i < party->num_parties; ++i) 
  {
    group_operation(preda->combined_Gamma, preda->combined_Gamma, party->parties[i]->presigning_data->Gamma, NULL, party->ec);
  }
  
  group_operation(preda->Delta, NULL, preda->combined_Gamma, preda->k, party->ec);

  zkp_aux_info_update(preda->aux, sizeof(hash_chunk), &party->id, sizeof(uint64_t));

  scalar_t alpha_j = scalar_new();

  scalar_mul(preda->delta, preda->gamma, preda->k, party->ec_order);
  scalar_mul(preda->chi, party->secret_x, preda->k, party->ec_order);

  for (uint64_t j = 0; j < party->num_parties; ++j) 
  {
    if (j == party->index) continue;
    
    // Compute delta_i
    paillier_encryption_decrypt(alpha_j, party->parties[j]->presigning_data->D_j[party->index], party->paillier_priv);
    scalar_make_plus_minus(alpha_j, party->paillier_priv->pub.N);
    scalar_add(preda->delta, preda->delta, alpha_j, party->ec_order);
    scalar_sub(preda->delta, preda->delta, preda->beta_j[j], party->ec_order);

    // Compute chi_i
    paillier_encryption_decrypt(alpha_j, party->parties[j]->presigning_data->Dhat_j[party->index], party->paillier_priv);
    scalar_make_plus_minus(alpha_j, party->paillier_priv->pub.N);
    scalar_add(preda->chi, preda->chi, alpha_j, party->ec_order);
    scalar_sub(preda->chi, preda->chi, preda->betahat_j[j], party->ec_order);

    // Create Group vs Paillier range ZKP for K against Gamma and Delta

    preda->psi_logK[j]->public.paillier_pub = &party->paillier_priv->pub;
    preda->psi_logK[j]->public.rped_pub = party->rped_pub[j];
    preda->psi_logK[j]->public.G = party->ec;
    preda->psi_logK[j]->public.g = preda->combined_Gamma;
    preda->psi_logK[j]->public.X = preda->Delta;
    preda->psi_logK[j]->public.C = preda->K;
    preda->psi_logK[j]->public.x_range_bytes = CALIGRAPHIC_I_ZKP_RANGE_BYTES;
    preda->psi_logK[j]->secret.x = preda->k;
    preda->psi_logK[j]->secret.rho = preda->rho;
    zkp_group_vs_paillier_range_prove(preda->psi_logK[j], preda->aux);
  }

  scalar_free(alpha_j);
  
  time_diff = (clock() - time_start) * 1000 /CLOCKS_PER_SEC;
  preda->run_time += time_diff;
  
  uint64_t psi_logK_bytes;
  zkp_group_vs_paillier_range_proof_to_bytes(NULL, &psi_logK_bytes, NULL, CALIGRAPHIC_I_ZKP_RANGE_BYTES, 0);

  printf("### Round 3. Party %lu publishes (sid, i, delta_i, Delta_i, psi_logK_j)).\t>>>\t%lu B, %lu ms\n", party->id, 
    2*sizeof(uint64_t) + GROUP_ORDER_BYTES + GROUP_ELEMENT_BYTES + (party->num_parties -1)*psi_logK_bytes, time_diff);

  if (!PRINT_VALUES) return;
  printf("delta_%lu = ", party->index); printBIGNUM("", preda->delta, "\n");
  printf("Delta_%lu = ", party->index); printECPOINT("", preda->Delta, party->ec, "\n", 1);
}

void  cmp_presigning_final_exec (cmp_party_t *party)
{
  clock_t time_start = clock();
  uint64_t time_diff;

  cmp_presigning_t *preda = party->presigning_data;
  
  // Verify ZKP

  int *verified_psi_logK = calloc(party->num_parties, sizeof(int));
  int verified_delta;

  for (uint64_t j = 0; j < party->num_parties; ++j)
  {
    if (j == party->index) continue; 

    zkp_aux_info_update(preda->aux, sizeof(hash_chunk), &party->parties_ids[j], sizeof(uint64_t));
    verified_psi_logK[j] = zkp_group_vs_paillier_range_verify(party->parties[j]->presigning_data->psi_logK[party->index], preda->aux);
    if (verified_psi_logK[j] != 1) printf("%sParty %lu: failed verification of psi_logK from Party %lu\n",ERR_STR, party->id, party->parties_ids[j]);
  }
  free(verified_psi_logK);

  scalar_t combined_delta = scalar_new();
  gr_elem_t gen_to_delta = group_elem_new(party->ec);
  gr_elem_t combined_Delta = group_elem_new(party->ec);
  
  scalar_set_ul(combined_delta, 0);
  group_operation(combined_Delta, NULL, NULL, NULL, party->ec);
  for (uint64_t i = 0; i < party->num_parties; ++i) 
  {
    scalar_add(combined_delta, combined_delta, party->parties[i]->presigning_data->delta, party->ec_order);
    group_operation(combined_Delta, combined_Delta, party->parties[i]->presigning_data->Delta, NULL, party->ec);
  }
  group_operation(gen_to_delta, NULL, party->ec_gen, combined_delta, party->ec);
  assert(PAILLIER_MODULUS_BYTES >= CALIGRAPHIC_J_ZKP_RANGE_BYTES);    // The following ZKP is valid when N is bigger then beta's range)
  verified_delta = group_elem_equal(gen_to_delta, combined_Delta, party->ec);
  if (verified_delta != 1) printf("%sParty %lu: failed equality of g^{delta} = combined_Delta\n",ERR_STR, party->id);

  scalar_inv(combined_delta, combined_delta, party->ec_order);
  group_operation(party->R, NULL, preda->combined_Gamma, combined_delta, party->ec);
  
  scalar_free(combined_delta);
  group_elem_free(combined_Delta);
  group_elem_free(gen_to_delta);

  scalar_copy(party->k, preda->k);
  scalar_copy(party->chi, preda->chi);
  
  time_diff = (clock() - time_start) * 1000 /CLOCKS_PER_SEC;
  preda->run_time += time_diff;
  
  printf("### Round 4. Party %lu stores (sid, i, R, k_i, chi_i).\t>>>\t%lu B, %lu ms\n", party->id, 
    2*sizeof(uint64_t) + 2*GROUP_ORDER_BYTES + GROUP_ELEMENT_BYTES, time_diff);

  if (!PRINT_VALUES) return;
  printf("R_%lu = ", party->index); printECPOINT("", party->R, party->ec, "\n", 1);  
  printf("k_%lu = ", party->index); printBIGNUM("", party->k, "\n");
  printf("chi_%lu = ", party->index); printBIGNUM("", party->chi, "\n");
}

void cmp_signature_share (scalar_t r, scalar_t sigma, const cmp_party_t *party, const scalar_t msg)
{
  scalar_t first_term = scalar_new();
  scalar_t second_term = scalar_new();

  group_elem_get_x(r, party->R, party->ec, party->ec_order);
  
  scalar_mul(first_term, party->k, msg, party->ec_order);
  scalar_mul(second_term, party->chi, r, party->ec_order);
  scalar_add(sigma, first_term, second_term, party->ec_order);
  
  scalar_free(first_term);
  scalar_free(second_term);
}