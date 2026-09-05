#include "CrowdyCppBridge.h"
#include "CrowdyCppCrypto.h"

#include <atomic>
#include <memory>
#include <mutex>

THIRD_PARTY_INCLUDES_START
#include "crowdy/core/crypto.hpp"
THIRD_PARTY_INCLUDES_END

// Silence OpenSSL 3.x deprecation of the one-shot SHA256() helper; the engine
// ships either 1.1.1 or 3.x and both provide these entry points.
#define OPENSSL_SUPPRESS_DEPRECATED

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#endif
THIRD_PARTY_INCLUDES_START
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END
#if PLATFORM_WINDOWS
#include "Windows/HideWindowsPlatformTypes.h"
#endif

// Binds CrowdyCPP's crypto interface to Unreal's bundled OpenSSL. Replaces the
// vendored provider (src/core/crypto_openssl.cpp is not compiled into the
// bridge). The SDK needs HMAC-SHA256 in both a one-shot and a pre-keyed form,
// plain SHA-256, constant-time compare, and secure random bytes.
namespace crowdy::core
{
	namespace
	{
		/**
		 * A MAC bound to one key. On the replication path a single token signs every datagram for the life of a
		 * session, so importing the key on each call dominates the cost of the hashing itself. Holding a keyed
		 * context and resetting it per message removes that.
		 *
		 * The library's own provider expresses this with OpenSSL 3.x's EVP_MAC. The engine bundles 1.1.1, where
		 * EVP_MAC and OSSL_PARAM do not exist at all, so this is the same design on HMAC_CTX: one keyed template
		 * that is never updated or finalised, duplicated once per thread, then reset per message with the key
		 * schedule intact.
		 */
		class FKeyedHmac final : public IMac
		{
		public:
			static std::shared_ptr<IMac> Create(const Bytes Key)
			{
				HMAC_CTX* Keyed = HMAC_CTX_new();
				if (Keyed == nullptr)
				{
					return nullptr;
				}

				if (HMAC_Init_ex(Keyed, Key.data(), static_cast<int>(Key.size()), EVP_sha256(), nullptr) != 1)
				{
					HMAC_CTX_free(Keyed);
					return nullptr;
				}
				return std::shared_ptr<IMac>(new FKeyedHmac(Keyed));
			}

			virtual ~FKeyedHmac() override
			{
				HMAC_CTX_free(Keyed);
			}

			bool compute(const Bytes* Parts, std::size_t Count, std::uint8_t* Out) const override
			{
				// One slot per thread rather than per (thread, key): two connections on different tokens alternating
				// on one thread re-duplicate each time, which costs about what the one-shot call did and is still
				// correct. One connection per thread, the ordinary case, duplicates once.
				thread_local FThreadContext Local;
				if (Local.Owner != Id)
				{
					if (!Local.AdoptCopyOf(*this))
					{
						return false;
					}
				}

				// Back to the keyed state without redoing the key schedule.
				if (HMAC_Init_ex(Local.Context, nullptr, 0, nullptr, nullptr) != 1)
				{
					return false;
				}

				for (std::size_t Index = 0; Index < Count; ++Index)
				{
					if (Parts[Index].empty())
					{
						continue;
					}
					if (HMAC_Update(Local.Context, Parts[Index].data(), Parts[Index].size()) != 1)
					{
						return false;
					}
				}

				unsigned int Written = 0;
				return HMAC_Final(Local.Context, Out, &Written) == 1 && Written == ICrypto::kHmacTagSize;
			}

		private:
			struct FThreadContext
			{
				std::uint64_t Owner = 0;
				HMAC_CTX* Context = nullptr;

				~FThreadContext()
				{
					if (Context != nullptr)
					{
						HMAC_CTX_free(Context);
					}
				}

				bool AdoptCopyOf(const FKeyedHmac& Source)
				{
					if (Context == nullptr)
					{
						Context = HMAC_CTX_new();
					}
					if (Context == nullptr)
					{
						Owner = 0;
						return false;
					}

					// 1.1.1 makes no concurrency promise about reading a shared source context, and every thread
					// copies from the same one. This is off the per-message path: it runs once per thread per key.
					bool bCopied = false;
					{
						std::lock_guard<std::mutex> Lock(Source.CopyMutex);
						bCopied = HMAC_CTX_copy(Context, Source.Keyed) == 1;
					}

					Owner = bCopied ? Source.Id : 0;
					return bCopied;
				}
			};

			explicit FKeyedHmac(HMAC_CTX* InKeyed)
				: Keyed(InKeyed)
				, Id(NextId())
			{
			}

			// Monotonic and never reused, so a freed instance cannot be mistaken for a new one that happens to land
			// at the same address while a thread still holds a context stamped with it.
			static std::uint64_t NextId()
			{
				static std::atomic<std::uint64_t> Counter{0};
				return Counter.fetch_add(1, std::memory_order_relaxed) + 1;
			}

			// Only ever init'ed, never updated or finalised, so it stays a pristine template to duplicate from.
			HMAC_CTX* Keyed;
			mutable std::mutex CopyMutex;
			std::uint64_t Id;
		};

		class FUnrealOpenSslCrypto final : public ICrypto
		{
		public:
			bool hmacSha256(Bytes key, Bytes message, std::uint8_t* out) const override
			{
				unsigned int OutLen = 0;
				const unsigned char* Result = HMAC(
					EVP_sha256(),
					key.data(), static_cast<int>(key.size()),
					message.data(), message.size(),
					out, &OutLen);
				return Result != nullptr && OutLen == kHmacTagSize;
			}

			std::shared_ptr<IMac> makeHmacSha256(Bytes key) const override
			{
				return FKeyedHmac::Create(key);
			}

			bool sha256(Bytes message, std::uint8_t* out) const override
			{
				return SHA256(message.data(), message.size(), out) != nullptr;
			}

			bool constantTimeEquals(const std::uint8_t* a, const std::uint8_t* b,
				std::size_t len) const override
			{
				return CRYPTO_memcmp(a, b, len) == 0;
			}

			bool randomBytes(std::uint8_t* out, std::size_t len) const override
			{
				return RAND_bytes(out, static_cast<int>(len)) == 1;
			}
		};
	}

	const ICrypto& opensslCrypto()
	{
		static FUnrealOpenSslCrypto Crypto;
		return Crypto;
	}

	// The library's build-selected provider. Upstream defines this alongside
	// whichever crypto source its build compiles; the bridge compiles neither of
	// those, so it must supply the symbol. Unreal always has OpenSSL, so this is
	// the OpenSSL provider above rather than the failing fallback.
	const ICrypto& defaultCrypto()
	{
		return opensslCrypto();
	}
}

// Routes through defaultCrypto() rather than opensslCrypto() directly so this exported accessor
// can never diverge from the library's own build-selected provider.
const crowdy::core::ICrypto& GetCrowdyCppCrypto()
{
	return crowdy::core::defaultCrypto();
}
