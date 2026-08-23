/* glibc-compatible rand()/srand() for the Nintendo 64 build.
 *
 * base/math.h builds random_int()/random_float() on top of the C library
 * rand(). newlib's generator is a different algorithm from glibc's, so a bot
 * match that ever consumed rand() would diverge from the host reference for a
 * reason that has nothing to do with the simulation. The current deathmatch
 * path does not reach rand() (team shuffling is a console command and
 * IGameController::EvaluateSpawnType only randomises in survival gametypes),
 * but replicating glibc removes the whole class of divergence up front.
 *
 * This is glibc's TYPE_3 additive-feedback generator (degree 31, separation 3)
 * exactly as implemented by random_r.c / srandom_r.c.
 */

#include <stdint.h>

#define TW_RAND_DEG 31
#define TW_RAND_SEP 3

static int32_t s_aState[TW_RAND_DEG];
static int s_Front = TW_RAND_SEP;
static int s_Rear = 0;
static int s_Seeded = 0;

static int32_t tw_random_step(void)
{
	uint32_t Value = (uint32_t)s_aState[s_Front] + (uint32_t)s_aState[s_Rear];
	s_aState[s_Front] = (int32_t)Value;
	int32_t Result = (int32_t)((Value >> 1) & 0x7fffffffu);
	if(++s_Front >= TW_RAND_DEG)
		s_Front = 0;
	if(++s_Rear >= TW_RAND_DEG)
		s_Rear = 0;
	return Result;
}

void srand(unsigned int Seed)
{
	int i;
	if(Seed == 0)
		Seed = 1;
	s_aState[0] = (int32_t)Seed;
	for(i = 1; i < TW_RAND_DEG; ++i)
	{
		/* Knuth's minimal standard generator, exactly as glibc seeds it:
		 * state[i] = (16807 * state[i-1]) % 2147483647 evaluated without
		 * overflowing 32 bits. */
		int32_t Previous = s_aState[i - 1];
		int32_t Hi = Previous / 127773;
		int32_t Lo = Previous % 127773;
		int32_t Word = 16807 * Lo - 2836 * Hi;
		if(Word < 0)
			Word += 2147483647;
		s_aState[i] = Word;
	}
	s_Front = TW_RAND_SEP;
	s_Rear = 0;
	s_Seeded = 1;
	for(i = 0; i < TW_RAND_DEG * 10; ++i)
		tw_random_step();
}

int rand(void)
{
	if(!s_Seeded)
		srand(1);
	return (int)tw_random_step();
}

long int random(void)
{
	return rand();
}

void srandom(unsigned int Seed)
{
	srand(Seed);
}
