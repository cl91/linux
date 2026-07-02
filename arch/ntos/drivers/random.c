#include <linux/string.h>
#include <linux/random.h>
#include <linux/notifier.h>
#include <crypto/chacha.h>

#ifdef __x86_64__
#define RDRAND_RETRY_LOOPS	10
static inline bool __must_check rdrand_long(unsigned long *v)
{
	bool ok;
	unsigned int retry = RDRAND_RETRY_LOOPS;
	do {
		asm volatile("rdrand %[out]"
			     : "=@ccc" (ok), [out] "=r" (*v));
		if (ok)
			return true;
	} while (--retry);
	return false;
}
#endif

void get_random_bytes(void *buf, size_t len)
{
	u8 *ptr = (u8 *)buf;

#ifdef __x86_64__
	/* ------------------------------------------------------------
	 * AMD64 (x86_64): Use hardware RDRAND
	 * ------------------------------------------------------------ */
	while (len > 0) {
		unsigned long val;

		// Try to get 8 bytes from RDRAND. If it fails (rare), we retry.
		if (rdrand_long(&val)) {
			size_t chunk = (len < sizeof(val)) ? len : sizeof(val);
			memcpy(ptr, &val, chunk);
			ptr += chunk;
			len -= chunk;
		} else {
			// Fallback just in case RDRAND fails transiently
			// (or CPU lacks it, though assumed present here)
			cpu_relax();
		}
	}

#else
	/* ------------------------------------------------------------
	 * i386 / ARM64: Simple ChaCha20 CSPRNG seeded by TSC
	 * ------------------------------------------------------------ */
	static bool seeded = false;
	static u32 key[8];
	static u64 counter = 0; // 64-bit block counter

	// Lazy initialization using TSC
	if (unlikely(!seeded)) {
		int i;
		for (i = 0; i < 8; i++) {
			key[i] = (u32)ktime_get();
		}
		seeded = true;
	}

	while (len > 0) {
		struct chacha_state state;
		u8 stream_output[CHACHA_BLOCK_SIZE];

		/*
		 * Manually populate the 16-word ChaCha state:
		 * Words 0..3:  ChaCha constants ("expand 32-byte k")
		 * Words 4..11: 256-bit key
		 * Words 12..13: 64-bit block counter
		 * Words 14..15: 64-bit nonce (we can just use 0 or part of the counter)
		 */

		// 1. Constants
		state.x[0] = 0x61707865;
		state.x[1] = 0x3320646e;
		state.x[2] = 0x79622d32;
		state.x[3] = 0x6b206574;

		// 2. Key
		memcpy(&state.x[4], key, sizeof(key));

		// 3. 64-bit Counter
		state.x[12] = (u32)(counter & 0xFFFFFFFF);
		state.x[13] = (u32)(counter >> 32);

		// 4. Nonce (Using 0 here is fine since the counter never repeats)
		state.x[14] = 0;
		state.x[15] = 0;

		// Generate the random block using the v7.0 inline function
		chacha20_block(&state, stream_output);

		counter++; // Increment block counter

		// Copy data out
		size_t chunk = (len < CHACHA_BLOCK_SIZE) ? len : CHACHA_BLOCK_SIZE;
		memcpy(ptr, stream_output, chunk);

		ptr += chunk;
		len -= chunk;

		// Mix the state back into the key to provide basic forward secrecy
		if (len > 0) {
			int i;
			for (i = 0; i < 8; i++) {
				key[i] ^= ((u32 *)stream_output)[i];
			}
		}
	}
#endif
}
EXPORT_SYMBOL(get_random_bytes);

#define DEFINE_GET_RANDOM(ty)				\
	ty get_random_##ty(void)			\
	{						\
		ty val;					\
		get_random_bytes(&val, sizeof(val));	\
		return val;				\
	}						\
	EXPORT_SYMBOL(get_random_##ty)

DEFINE_GET_RANDOM(u8);
DEFINE_GET_RANDOM(u16);
DEFINE_GET_RANDOM(u32);
DEFINE_GET_RANDOM(u64);

u32 __get_random_u32_below(u32 ceil)
{
	/*
	 * This is the slow path for variable ceil. It is still fast, most of
	 * the time, by doing traditional reciprocal multiplication and
	 * opportunistically comparing the lower half to ceil itself, before
	 * falling back to computing a larger bound, and then rejecting samples
	 * whose lower half would indicate a range indivisible by ceil. The use
	 * of `-ceil % ceil` is analogous to `2^32 % ceil`, but is computable
	 * in 32-bits.
	 */
	u32 rand = get_random_u32();
	u64 mult;

	/*
	 * This function is technically undefined for ceil == 0, and in fact
	 * for the non-underscored constant version in the header, we build bug
	 * on that. But for the non-constant case, it's convenient to have that
	 * evaluate to being a straight call to get_random_u32(), so that
	 * get_random_u32_inclusive() can work over its whole range without
	 * undefined behavior.
	 */
	if (unlikely(!ceil))
		return rand;

	mult = (u64)ceil * rand;
	if (unlikely((u32)mult < ceil)) {
		u32 bound = -ceil % ceil;
		while (unlikely((u32)mult < bound))
			mult = (u64)ceil * get_random_u32();
	}
	return mult >> 32;
}
EXPORT_SYMBOL(__get_random_u32_below);

int __cold execute_with_initialized_rng(struct notifier_block *nb)
{
	if (nb && nb->notifier_call) {
		nb->notifier_call(nb, 0, NULL);
	}
	return 0;
}

/*
 * Add device- or boot-specific data to the input pool to help
 * initialize it.
 *
 * None of this adds any entropy; it is meant to avoid the problem of
 * the entropy pool having similar initial state across largely
 * identical devices.
 */
void add_device_randomness(const void *buf, size_t len)
{
	/* Do nothing */
}
EXPORT_SYMBOL(add_device_randomness);
