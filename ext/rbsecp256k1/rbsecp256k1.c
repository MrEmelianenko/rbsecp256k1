// rbsecp256k1.c - Ruby VM interfaces for library.
//
// Description:
// This library provides a low-level and high-performance Ruby wrapper around
// libsecp256k1. It includes functions for generating key pairs, signing data,
// and verifying signatures using the library.
//
// Dependencies:
// * libsecp256k1
// * openssl
#include <ruby.h>

#include <openssl/rand.h>
#include <openssl/sha.h>
#include <secp256k1.h>

// High-level design:
//
// The Ruby wrapper is divided into the following hierarchical organization:
//
// +- Secp256k1 (Top-level module)
// |--  Context
// |--  KeyPair
// |--  PublicKey
// |--  PrivateKey
// |--  Signature
//
// The Context class contains most of the methods that invoke libsecp256k1.
// The KayPair, PublicKey, PrivateKey, and Signature objects act as data
// objects and are passed to various methods. Contexts are thread safe and can
// be used across applications. Context initialization is expensive so it is
// recommended that a single context be initialized and used throughout an
// application when possible.

//
// The section below contains purely internal methods used exclusively by the
// C internals of the library.
//

// Size of an uncompressed public key
const size_t UNCOMPRESSED_PUBKEY_SIZE_BYTES = 65;
// Size of a compressed public key
const size_t COMPRESSED_PUBKEY_SIZE_BYTES = 33;
// Size of a compact signature in bytes
const size_t COMPACT_SIG_SIZE_BYTES = 64;

// Globally define our module and its associated classes so we can instantiate
// objects from anywhere. The use of global variables seems to be inline with
// how the Ruby project builds its own extension gems.
static VALUE Secp256k1_module;
static VALUE Secp256k1_Context_class;
static VALUE Secp256k1_KeyPair_class;
static VALUE Secp256k1_PublicKey_class;
static VALUE Secp256k1_PrivateKey_class;
static VALUE Secp256k1_Signature_class;

// Forward definitions for all structures
typedef struct Context_dummy {
  secp256k1_context *ctx; // Context used by libsecp256k1 library
} Context;

typedef struct KeyPair_dummy {
  VALUE public_key;
  VALUE private_key;
} KeyPair;

typedef struct PublicKey_dummy {
  secp256k1_pubkey pubkey; // Opaque object containing public key data
  secp256k1_context *ctx;
} PublicKey;

typedef struct PrivateKey_dummy {
  unsigned char data[32]; // Bytes comprising the private key data
  secp256k1_context *ctx;
} PrivateKey;

typedef struct Signature_dummy {
  secp256k1_ecdsa_signature sig; // Signature object, contains 64-byte signature
  secp256k1_context *ctx;
} Signature;

//
// Typed data definitions
//

// Context
static void
Context_free(void* in_context)
{
  Context *context;
  context = (Context*)in_context;
  secp256k1_context_destroy(context->ctx);
  xfree(context);
}

static const rb_data_type_t Context_DataType = {
  "Context",
  { 0, Context_free, 0 },
  0, 0,
  RUBY_TYPED_FREE_IMMEDIATELY
};

// PublicKey
static void
PublicKey_free(void *in_public_key)
{
  PublicKey *public_key;
  public_key = (PublicKey*)in_public_key;
  secp256k1_context_destroy(public_key->ctx);
  xfree(public_key);
}

static const rb_data_type_t PublicKey_DataType = {
  "PublicKey",
  { 0, PublicKey_free, 0 },
  0, 0,
  RUBY_TYPED_FREE_IMMEDIATELY
};

// PrivateKey
static void
PrivateKey_free(void *in_private_key)
{
  PrivateKey *private_key;
  private_key = (PrivateKey*)in_private_key;
  secp256k1_context_destroy(private_key->ctx);
  xfree(private_key);
}

static const rb_data_type_t PrivateKey_DataType = {
  "PrivateKey",
  { 0, PrivateKey_free, 0 },
  0, 0,
  RUBY_TYPED_FREE_IMMEDIATELY
};

// KeyPair
static void
KeyPair_mark(void *in_key_pair)
{
  KeyPair *key_pair = (KeyPair*)in_key_pair;

  // Mark both contained objects to ensure they are properly garbage collected
  rb_gc_mark(key_pair->public_key);
  rb_gc_mark(key_pair->private_key);
}

static void
KeyPair_free(void *self)
{
  xfree(self);
}

static const rb_data_type_t KeyPair_DataType = {
  "KeyPair",
  { KeyPair_mark, KeyPair_free, 0 },
  0, 0,
  RUBY_TYPED_FREE_IMMEDIATELY
};

// Signature
static void
Signature_free(void *in_signature)
{
  Signature *signature = (Signature*)in_signature;
  secp256k1_context_destroy(signature->ctx);
  xfree(signature);
}

static const rb_data_type_t Signature_DataType = {
  "Signature",
  { 0, Signature_free, 0 },
  0, 0,
  RUBY_TYPED_FREE_IMMEDIATELY
};

/**
 * Macro: SUCCESS
 * 
 * Determines whether or not the given function result was a success.
 */
#define SUCCESS(x) ((x) == RESULT_SUCCESS)

/**
 * Macro: FAILURE
 *
 * Indicates whether or not the given function result is a failure.
 */
#define FAILURE(x) !SUCCESS(x)

/* Result type for internally defined functions */
typedef enum ResultT_dummy {
  RESULT_SUCCESS,
  RESULT_FAILURE
} ResultT;

/**
 * Generate a series of cryptographically secure random bytes using OpenSSL.
 *
 * \param out_bytes Desired number of bytes will be written here.
 * \param in_size Number of bytes of random data to be generated.
 * \return RESULT_SUCCESS if the bytes were generated successfully,
 *   RESULT_FAILURE otherwise.
 */
static ResultT
GenerateRandomBytes(unsigned char *out_bytes, size_t in_size)
{
  // OpenSSL RNG has not been seeded with enough data and is therefore
  // not usable.
  if (RAND_status() == 0)
  {
    return RESULT_FAILURE;
  }

  // Attempt to generate random bytes using the OpenSSL RNG
  if (RAND_bytes(out_bytes, in_size) != 1)
  {
    return RESULT_FAILURE;
  }

  return RESULT_SUCCESS;
}

/**
 * Computes the ECDSA signature of the given data.
 *
 * This method first computes the ECDSA signature of the given data (can be
 * text or binary data) and outputs both the raw libsecp256k1 signature.
 *
 * ECDSA signing involves the following steps:
 *   1. Compute the 32-byte SHA-256 hash of the given data.
 *   2. Sign the 32-byte hash using the private key provided.
 *
 * \param in_context libsecp256k1 context
 * \param in_data Data to be signed
 * \param in_data_len Length of data to be signed
 * \param in_private_key Private key to be used for signing
 * \param out_signature Signature produced during the signing proccess
 * \return RESULT_SUCCESS if the hash and signature were computed successfully,
 *   RESULT_FAILURE if signing failed or DER encoding failed.
 */
static ResultT
SignData(secp256k1_context *in_context,
         unsigned char *in_data,
         unsigned long in_data_len,
         unsigned char *in_private_key,
         secp256k1_ecdsa_signature *out_signature)
{
  unsigned char hash[SHA256_DIGEST_LENGTH];

  // Compute the SHA-256 hash of data
  SHA256(in_data, in_data_len, hash);

  // Sign the hash of the data
  if (secp256k1_ecdsa_sign(in_context,
                           out_signature,
                           hash,
                           in_private_key,
                           NULL,
                           NULL) == 1)
  {
    return RESULT_SUCCESS;
  }

  return RESULT_FAILURE;
}

//
// Secp256k1::PublicKey class interface
//

static VALUE
PublicKey_alloc(VALUE klass)
{
  VALUE result;
  PublicKey *public_key;

  public_key = ALLOC(PublicKey);
  MEMZERO(public_key, PublicKey, 1);
  result = TypedData_Wrap_Struct(klass, &PublicKey_DataType, public_key);

  return result;
}

/**
 * Initialize a new public key from the given context and private key.
 *
 * @param in_context [Secp256k1::Context] context instance to be used in derivation
 * @param in_private_key [Secp256k1::PrivateKey] private key to derive public key from
 * @return [Secp256k1::PublicKey] public key derived from private key
 * @raise [TypeError] if private key data is invalid
 */
static VALUE
PublicKey_initialize(VALUE self, VALUE in_context, VALUE in_private_key)
{
  Context *context;
  PublicKey *public_key;
  PrivateKey *private_key;

  TypedData_Get_Struct(self, PublicKey, &PublicKey_DataType, public_key);
  TypedData_Get_Struct(in_context, Context, &Context_DataType, context);
  TypedData_Get_Struct(in_private_key, PrivateKey, &PrivateKey_DataType, private_key);

  if (secp256k1_ec_pubkey_create(context->ctx,
                                 &(public_key->pubkey),
                                 private_key->data) == 0)
  {
    rb_raise(rb_eTypeError, "Invalid private key data");
  }

  public_key->ctx = secp256k1_context_clone(context->ctx);

  return self;
}

/**
 * @return [String] binary string containing the uncompressed representation
 *   of this public key.
 */
static VALUE
PublicKey_uncompressed(VALUE self)
{
  // TODO: Cache value after first computation
  PublicKey *public_key;
  size_t serialized_pubkey_len = UNCOMPRESSED_PUBKEY_SIZE_BYTES;
  unsigned char serialized_pubkey[UNCOMPRESSED_PUBKEY_SIZE_BYTES];

  TypedData_Get_Struct(self, PublicKey, &PublicKey_DataType, public_key);

  secp256k1_ec_pubkey_serialize(public_key->ctx,
                                serialized_pubkey,
                                &serialized_pubkey_len,
                                &(public_key->pubkey),
                                SECP256K1_EC_UNCOMPRESSED);

  return rb_str_new((char*)serialized_pubkey, serialized_pubkey_len);
}

/**
 * @return [String] binary string containing the compressed representation of
 *   this public key.
 */
static VALUE
PublicKey_compressed(VALUE self)
{
  // TODO: Cache value after first computation
  PublicKey *public_key;
  size_t serialized_pubkey_len = COMPRESSED_PUBKEY_SIZE_BYTES;
  unsigned char serialized_pubkey[COMPRESSED_PUBKEY_SIZE_BYTES];

  TypedData_Get_Struct(self, PublicKey, &PublicKey_DataType, public_key);

  secp256k1_ec_pubkey_serialize(public_key->ctx,
                                serialized_pubkey,
                                &serialized_pubkey_len,
                                &(public_key->pubkey),
                                SECP256K1_EC_COMPRESSED);

  return rb_str_new((char*)serialized_pubkey, serialized_pubkey_len);
}

//
// Secp256k1::PrivateKey class interface
//

/* Allocate space for new private key internal data */
static VALUE
PrivateKey_alloc(VALUE klass)
{
  VALUE new_instance;
  PrivateKey *private_key;

  private_key = ALLOC(PrivateKey);
  MEMZERO(private_key, PrivateKey, 1);
  new_instance = TypedData_Wrap_Struct(klass, &PrivateKey_DataType, private_key);

  return new_instance;
}

/**
 * Generates a new random private key.
 *
 * @param in_context [Secp256k1::Context] context to be used in private key
 *   generation.
 * @return [Secp256k1::PrivateKey] new, randomly generated private key.
 */
static VALUE
PrivateKey_generate(VALUE klass, VALUE in_context)
{
  unsigned char private_key_bytes[32];

  if (FAILURE(GenerateRandomBytes(private_key_bytes, 32)))
  {
    rb_raise(rb_eRuntimeError, "Random bytes generation failed.");
  }

  return rb_funcall(
    klass,
    rb_intern("new"),
    2,
    in_context,
    rb_str_new((char*)private_key_bytes, 32)
  );
}

/**
 * Initialize a new private key from binary data.
 *
 * @param in_context [Secp256k1::Context] context to be used in private key
 *   generation.
 * @param in_private_key_data [String] binary string with 32 bytes of private
 *   key data.
 * @raise [ArgumentError] if private key data is not 32 bytes long or is
 *   invalid.
 */
static VALUE
PrivateKey_initialize(VALUE self, VALUE in_context, VALUE in_private_key_data)
{
  PrivateKey *private_key;
  Context *context;
  unsigned char *private_key_data;

  Check_Type(in_private_key_data, T_STRING);
  TypedData_Get_Struct(in_context, Context, &Context_DataType, context);
  private_key_data = (unsigned char*)StringValuePtr(in_private_key_data);

  if (RSTRING_LEN(in_private_key_data) != 32)
  {
    rb_raise(rb_eArgError, "private key data must be 32 bytes in length");
  }

  if (secp256k1_ec_seckey_verify(context->ctx, private_key_data) != 1)
  {
    rb_raise(rb_eArgError, "invalid private key data");
  }    

  TypedData_Get_Struct(self, PrivateKey, &PrivateKey_DataType, private_key);
  MEMCPY(private_key->data, private_key_data, char, 32);
  private_key->ctx = secp256k1_context_clone(context->ctx);

  // Set the PrivateKey.data attribute for later reading
  rb_iv_set(self, "@data", in_private_key_data);

  return self;
}

//
// Secp256k1::Signature class interface
//

/* Allocate memory for Signature object */
static VALUE
Signature_alloc(VALUE klass)
{
  VALUE new_instance;
  Signature *signature;

  signature = ALLOC(Signature);
  MEMZERO(signature, Signature, 1);
  new_instance = TypedData_Wrap_Struct(klass, &Signature_DataType, signature);

  return new_instance;
}

/**
 * Return Distinguished Encoding Rules (DER) encoded signature data.
 *
 * @return [String] binary string containing DER-encoded signature data.
 */
static VALUE
Signature_der_encoded(VALUE self)
{
  // TODO: Cache value after first computation
  Signature *signature;
  unsigned long der_signature_len;
  unsigned char der_signature[512];

  TypedData_Get_Struct(self, Signature, &Signature_DataType, signature);

  der_signature_len = 512;
  if (secp256k1_ecdsa_signature_serialize_der(signature->ctx,
                                              der_signature,
                                              &der_signature_len,
                                              &(signature->sig)) != 1)
  {
    rb_raise(rb_eRuntimeError, "could not compute DER encoded signature");
  }

  return rb_str_new((char*)der_signature, der_signature_len);
}

/**
 * Returns the 64 byte compact representation of this signature.
 *
 * @return [String] 64 byte binary string containing signature data.
 */
static VALUE
Signature_compact(VALUE self)
{
  // TODO: Cache value after first computation
  Signature *signature;
  unsigned char compact_signature[COMPACT_SIG_SIZE_BYTES];

  TypedData_Get_Struct(self, Signature, &Signature_DataType, signature);

  if (secp256k1_ecdsa_signature_serialize_compact(signature->ctx,
                                                  compact_signature,
                                                  &(signature->sig)) != 1)
  {
    rb_raise(rb_eRuntimeError, "unable to compute compact signature");
  }

  return rb_str_new((char*)compact_signature, COMPACT_SIG_SIZE_BYTES);
}

//
// Secp256k1::Context class interface
//

/* Allocate a new context object */
static VALUE
Context_alloc(VALUE klass)
{
  VALUE new_instance;
  Context *context;

  context = ALLOC(Context);
  MEMZERO(context, Context, 1);

  new_instance = TypedData_Wrap_Struct(klass, &Context_DataType, context);

  return new_instance;
}

/**
 * Initialize a new context.
 *
 * Context initialization should be infrequent as it is an expensive operation.
 *
 * @raise [RuntimeError] if context randomization fails.
 */
static VALUE
Context_initialize(VALUE self)
{
  Context *context;
  unsigned char seed[32];

  TypedData_Get_Struct(self, Context, &Context_DataType, context);

  context->ctx = secp256k1_context_create(
    SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY
  );

  // Randomize the context at initialization time rather than before calls so
  // the same context can be used across threads safely.
  GenerateRandomBytes(seed, 32);
  if (secp256k1_context_randomize(context->ctx, seed) != 1)
  {
    rb_raise(rb_eRuntimeError, "context randomization failed");
  }

  return self;
}

/**
 * Generate a new public-private key pair.
 *
 * @return [Secp256k1::KeyPair] newly generated key pair.
 */
static VALUE
Context_generate_key_pair(VALUE self)
{
  VALUE private_key;
  VALUE public_key;
  VALUE key_pair;

  private_key = PrivateKey_generate(Secp256k1_PrivateKey_class, self);
  public_key = rb_funcall(Secp256k1_PublicKey_class,
                          rb_intern("new"),
                          2,
                          self,
                          private_key);
  key_pair = rb_funcall(Secp256k1_KeyPair_class,
                        rb_intern("new"),
                        2,
                        public_key,
                        private_key);

  return key_pair;
}

/**
 * Loads a public key from compressed or uncompressed binary data.
 *
 * @param in_public_key_data [String] binary string with compressed or
 *   uncompressed public key data.
 * @return [Secp256k1::PublicKey] public key derived from data.
 * @raise [RuntimeError] if public key data is invalid.
 */
static VALUE
Context_public_key_from_data(VALUE self, VALUE in_public_key_data)
{
  Context *context;
  PublicKey *public_key;
  unsigned char *public_key_data;
  VALUE result;

  Check_Type(in_public_key_data, T_STRING);

  TypedData_Get_Struct(self, Context, &Context_DataType, context);
  public_key_data = (unsigned char*)StringValuePtr(in_public_key_data);

  // TODO: Use public key constructor instead?
  public_key = ALLOC(PublicKey);
  MEMZERO(public_key, PublicKey, 1);
  public_key->ctx = secp256k1_context_clone(context->ctx);
  result = TypedData_Wrap_Struct(Secp256k1_PublicKey_class, &PublicKey_DataType, public_key);

  if (secp256k1_ec_pubkey_parse(context->ctx,
                                &(public_key->pubkey),
                                public_key_data,
                                RSTRING_LEN(in_public_key_data)) != 1)
  {
    rb_raise(rb_eRuntimeError, "invalid public key data");
  }

  return result;
}

/**
 * Converts binary private key data into a new key pair.
 *
 * @param in_private_key_data [String] binary private key data
 * @return [Secp256k1::KeyPair] key pair initialized from the private key data.
 * @raise [ArgumentError] if the private key data is invalid or key derivation
 *   fails.
 */
static VALUE
Context_key_pair_from_private_key(VALUE self, VALUE in_private_key_data)
{
  VALUE public_key;
  VALUE private_key;
  VALUE key_pair;

  private_key = rb_funcall(Secp256k1_PrivateKey_class,
                           rb_intern("new"),
                           2,
                           self,
                           in_private_key_data);
  public_key = rb_funcall(Secp256k1_PublicKey_class,
                          rb_intern("new"),
                          2,
                          self,
                          private_key);
  key_pair = rb_funcall(Secp256k1_KeyPair_class,
                        rb_intern("new"),
                        2,
                        public_key,
                        private_key);

  return key_pair;
}

/**
 * Converts a DER encoded binary signature into a signature object.
 *
 * @param in_der_encoded_signature [String] DER encoded signature as binary
 *   string.
 * @return [Secp256k1::Signature] signature object initialized using signature
 *   data.
 * @raise [ArgumentError] if signature data is invalid.
 */
static VALUE
Context_signature_from_der_encoded(VALUE self, VALUE in_der_encoded_signature)
{
  Context *context;
  Signature *signature;
  VALUE signature_result;
  unsigned char *signature_data;

  Check_Type(in_der_encoded_signature, T_STRING);

  TypedData_Get_Struct(self, Context, &Context_DataType, context);
  signature_data = (unsigned char*)StringValuePtr(in_der_encoded_signature);

  signature_result = Signature_alloc(Secp256k1_Signature_class);
  TypedData_Get_Struct(signature_result, Signature, &Signature_DataType, signature);

  if (secp256k1_ecdsa_signature_parse_der(context->ctx,
                                          &(signature->sig),
                                          signature_data,
                                          RSTRING_LEN(in_der_encoded_signature)) != 1)
  {
    rb_raise(rb_eArgError, "invalid DER encoded signature");
  }

  signature->ctx = secp256k1_context_clone(context->ctx);
  return signature_result;
}

/**
 * Deserializes a Signature from 64-byte compact signature data.
 *
 * @param in_compact_signature [String] compact signature as 64-byte binary
 *   string.
 * @return [Secp256k1::Signature] object deserialized from compact signature.
 * @raise [ArgumentError] if signature data is invalid.
 */
static VALUE
Context_signature_from_compact(VALUE self, VALUE in_compact_signature)
{
  Context *context;
  Signature *signature;
  VALUE signature_result;
  unsigned char *signature_data;

  TypedData_Get_Struct(self, Context, &Context_DataType, context);
  signature_data = (unsigned char*)StringValuePtr(in_compact_signature);

  signature_result = Signature_alloc(Secp256k1_Signature_class);
  TypedData_Get_Struct(signature_result, Signature, &Signature_DataType, signature);

  if (secp256k1_ecdsa_signature_parse_compact(context->ctx,
                                              &(signature->sig),
                                              signature_data) != 1)
  {
    rb_raise(rb_eArgError, "invalid compact signature");
  }

  signature->ctx = secp256k1_context_clone(context->ctx);
  return signature_result;
}

/**
 * Computes the ECDSA signature of the data using the secp256k1 elliptic curve.
 *
 * @param in_private_key [Secp256k1::PrivateKey] private key to use for
 *   signing.
 * @param in_data [String] binary or text data to be signed.
 * @raise [RuntimeError] if signature computation fails.
 */
static VALUE
Context_sign(VALUE self, VALUE in_private_key, VALUE in_data)
{
  unsigned char *data_ptr;
  PrivateKey *private_key;
  Context *context;
  Signature *signature;
  VALUE signature_result;

  Check_Type(in_data, T_STRING);

  TypedData_Get_Struct(self, Context, &Context_DataType, context);
  TypedData_Get_Struct(in_private_key, PrivateKey, &PrivateKey_DataType, private_key);
  data_ptr = (unsigned char*)StringValuePtr(in_data);

  signature_result = Signature_alloc(Secp256k1_Signature_class);
  TypedData_Get_Struct(signature_result, Signature, &Signature_DataType, signature);
 
  // Attempt to sign the hash of the given data
  if (SUCCESS(SignData(context->ctx,
                       data_ptr,
                       RSTRING_LEN(in_data),
                       private_key->data,
                       &(signature->sig))))
  {
    signature->ctx = secp256k1_context_clone(context->ctx);
    return signature_result;
  }

  rb_raise(rb_eRuntimeError, "unable to compute signature");
}

/**
 * Verifies that signature matches public key and data.
 *
 * @param in_signature [Secp256k1::Signature] signature to be verified.
 * @param in_pubkey [Secp256k1::PublicKey] public key to verify signature
 *   against.
 * @param in_message [String] text or binary data to verify signature against.
 * @return [Bool] True if the signature is valid, false otherwise.
 */
static VALUE
Context_verify(VALUE self, VALUE in_signature, VALUE in_pubkey, VALUE in_message)
{
  Context *context;
  PublicKey *public_key;
  Signature *signature;
  unsigned char *message_ptr;
  unsigned char hash[SHA256_DIGEST_LENGTH];

  Check_Type(in_message, T_STRING);

  TypedData_Get_Struct(self, Context, &Context_DataType, context);
  TypedData_Get_Struct(in_pubkey, PublicKey, &PublicKey_DataType, public_key);
  TypedData_Get_Struct(in_signature, Signature, &Signature_DataType, signature);

  message_ptr = (unsigned char*)StringValuePtr(in_message);
  SHA256(message_ptr, RSTRING_LEN(in_message), hash);
  
  if (secp256k1_ecdsa_verify(context->ctx,
                             &(signature->sig),
                             hash,
                             &(public_key->pubkey)) == 1)
  {
    return Qtrue;
  }

  return Qfalse;
}

//
// Secp256k1::KeyPair class interface
//

static VALUE
KeyPair_alloc(VALUE klass)
{
  KeyPair *key_pair;

  key_pair = ALLOC(KeyPair);

  return TypedData_Wrap_Struct(klass, &KeyPair_DataType, key_pair);
}

/**
 * Default constructor.
 *
 * @param in_public_key [Secp256k1::PublicKey] public key
 * @param in_private_key [Secp256k1::PrivateKey] private key
 * @return [Secp256k1::KeyPair] newly initialized key pair.
 */
static VALUE
KeyPair_initialize(VALUE self, VALUE in_public_key, VALUE in_private_key)
{
  KeyPair *key_pair;

  TypedData_Get_Struct(self, KeyPair, &KeyPair_DataType, key_pair);
  Check_TypedStruct(in_public_key, &PublicKey_DataType);
  Check_TypedStruct(in_private_key, &PrivateKey_DataType);

  key_pair->public_key = in_public_key;
  key_pair->private_key = in_private_key;

  rb_iv_set(self, "@public_key", in_public_key);
  rb_iv_set(self, "@private_key", in_private_key);

  return self;
}

// Data type definitions

//
// Library initialization
//

void Init_rbsecp256k1()
{
  // Secp256k1
  Secp256k1_module = rb_define_module("Secp256k1");

  // Secp256k1::Context
  Secp256k1_Context_class = rb_define_class_under(
    Secp256k1_module, "Context", rb_cObject
  );
  rb_define_alloc_func(Secp256k1_Context_class, Context_alloc);
  rb_define_method(Secp256k1_Context_class,
                   "initialize",
                   Context_initialize,
                   0);
  rb_define_method(Secp256k1_Context_class,
                   "generate_key_pair",
                   Context_generate_key_pair,
                   0);
  rb_define_method(Secp256k1_Context_class,
                   "key_pair_from_private_key",
                   Context_key_pair_from_private_key,
                   1);
  rb_define_method(Secp256k1_Context_class,
                   "public_key_from_data",
                   Context_public_key_from_data,
                   1);
  rb_define_method(Secp256k1_Context_class,
                   "sign",
                   Context_sign,
                   2);
  rb_define_method(Secp256k1_Context_class,
                   "verify",
                   Context_verify,
                   3);
  rb_define_method(Secp256k1_Context_class,
                   "signature_from_der_encoded",
                   Context_signature_from_der_encoded,
                   1);
  rb_define_method(Secp256k1_Context_class,
                   "signature_from_compact",
                   Context_signature_from_compact,
                   1);

  // Secp256k1::KeyPair
  Secp256k1_KeyPair_class = rb_define_class_under(Secp256k1_module,
                                                  "KeyPair",
                                                  rb_cObject);
  rb_define_alloc_func(Secp256k1_KeyPair_class, KeyPair_alloc);
  rb_define_attr(Secp256k1_KeyPair_class, "public_key", 1, 0);
  rb_define_attr(Secp256k1_KeyPair_class, "private_key", 1, 0);
  rb_define_method(Secp256k1_KeyPair_class,
                   "initialize",
                   KeyPair_initialize,
                   2);

  // Secp256k1::PublicKey
  Secp256k1_PublicKey_class = rb_define_class_under(Secp256k1_module,
                                                    "PublicKey",
                                                    rb_cObject);
  rb_define_alloc_func(Secp256k1_PublicKey_class, PublicKey_alloc);
  rb_define_method(Secp256k1_PublicKey_class,
                   "initialize",
                   PublicKey_initialize,
                   2);
  rb_define_method(Secp256k1_PublicKey_class,
                   "compressed",
                   PublicKey_compressed,
                   0);
  rb_define_method(Secp256k1_PublicKey_class,
                   "uncompressed",
                   PublicKey_uncompressed,
                   0);

  // Secp256k1::PrivateKey
  Secp256k1_PrivateKey_class = rb_define_class_under(
    Secp256k1_module, "PrivateKey", rb_cObject
  );
  rb_define_alloc_func(Secp256k1_PrivateKey_class, PrivateKey_alloc);
  rb_define_singleton_method(Secp256k1_PrivateKey_class,
                             "generate",
                             PrivateKey_generate,
                             1);
  rb_define_attr(Secp256k1_PrivateKey_class, "data", 1, 0);
  rb_define_method(Secp256k1_PrivateKey_class,
                   "initialize",
                   PrivateKey_initialize,
                   2);

  // Secp256k1::Signature
  Secp256k1_Signature_class = rb_define_class_under(Secp256k1_module,
                                                    "Signature",
                                                    rb_cObject);
  rb_define_alloc_func(Secp256k1_Signature_class, Signature_alloc);
  rb_define_method(Secp256k1_Signature_class,
                   "der_encoded",
                   Signature_der_encoded,
                   0);
  rb_define_method(Secp256k1_Signature_class,
                   "compact",
                   Signature_compact,
                   0);
}
