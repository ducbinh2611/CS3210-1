#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <math.h>
#include "util.h"
#include "exporter.h"
#include "settings.h"

// including the "dead faction": 0
#define MAX_FACTIONS 10

// this macro is here to make the code slightly more readable, not because it can be safely changed to
// any integer value; changing this to a non-zero value may break the code
#define DEAD_FACTION 0

/**
 * Specifies the number(s) of live neighbors of the same faction required for a dead cell to become alive.
 */
bool isBirthable(int n)
{
    return n == 3;
}

/**
 * Specifies the number(s) of live neighbors of the same faction required for a live cell to remain alive.
 */
bool isSurvivable(int n)
{
    return n == 2 || n == 3;
}

/**
 * Specifies the number of live neighbors of a different faction required for a live cell to die due to fighting.
 */
bool willFight(int n) {
    return n > 0;
}

/**
 * Computes and returns the next state of the cell specified by row and col based on currWorld and invaders. Sets *diedDueToFighting to
 * true if this cell should count towards the death toll due to fighting.
 * 
 * invaders can be NULL if there are no invaders.
 */
int getNextState(const int *currWorld, const int *invaders, int nRows, int nCols, int row, int col, bool *diedDueToFighting)
{
    // we'll explicitly set if it was death due to fighting
    *diedDueToFighting = false;

    // faction of this cell
    int cellFaction = getValueAt(currWorld, nRows, nCols, row, col);

    // did someone just get landed on?
    if (invaders != NULL && getValueAt(invaders, nRows, nCols, row, col) != DEAD_FACTION)
    {
        *diedDueToFighting = cellFaction != DEAD_FACTION;
        return getValueAt(invaders, nRows, nCols, row, col);
    }

    // tracks count of each faction adjacent to this cell
    int neighborCounts[MAX_FACTIONS];
    memset(neighborCounts, 0, MAX_FACTIONS * sizeof(int));

    // count neighbors (and self)
    for (int dy = -1; dy <= 1; dy++)
    {
        for (int dx = -1; dx <= 1; dx++)
        {
            int faction = getValueAt(currWorld, nRows, nCols, row + dy, col + dx);
            if (faction >= DEAD_FACTION)
            {
                neighborCounts[faction]++;
            }
        }
    }

    // we counted this cell as its "neighbor"; adjust for this
    neighborCounts[cellFaction]--;

    if (cellFaction == DEAD_FACTION)
    {
        // this is a dead cell; we need to see if a birth is possible:
        // need exactly 3 of a single faction; we don't care about other factions

        // by default, no birth
        int newFaction = DEAD_FACTION;

        // start at 1 because we ignore dead neighbors
        for (int faction = DEAD_FACTION + 1; faction < MAX_FACTIONS; faction++)
        {
            int count = neighborCounts[faction];
            if (isBirthable(count))
            {
                newFaction = faction;
            }
        }

        return newFaction;
    }
    else
    {
        /** 
         * this is a live cell; we follow the usual rules:
         * Death (fighting): > 0 hostile neighbor
         * Death (underpopulation): < 2 friendly neighbors and 0 hostile neighbors
         * Death (overpopulation): > 3 friendly neighbors and 0 hostile neighbors
         * Survival: 2 or 3 friendly neighbors and 0 hostile neighbors
         */

        int hostileCount = 0;
        for (int faction = DEAD_FACTION + 1; faction < MAX_FACTIONS; faction++)
        {
            if (faction == cellFaction)
            {
                continue;
            }
            hostileCount += neighborCounts[faction];
        }

        if (willFight(hostileCount))
        {
            *diedDueToFighting = true;
            return DEAD_FACTION;
        }

        int friendlyCount = neighborCounts[cellFaction];
        if (!isSurvivable(friendlyCount))
        {
            return DEAD_FACTION;
        }

        return cellFaction;
    }
}

int getRow(int nRows, int nCols, int index) {
    return index / nCols;
}

int getCol(int nRows, int nCols, int index) {
    return index % nCols;
}

typedef struct sharedStruct {
    pthread_mutex_t* mutex;
    int* world;
    int* inv;
    int nRows;
    int nCols;
    int startIdx;
    int endIdx;
    int* wholeNewWorld;
    int* deathToll;
    int iteration;
    int tid;
    pthread_mutex_t* isReady;
    int totalIteration;
    pthread_barrier_t* barrier;
} shared;

void* subroutine(void* sharedStruct) {
    shared* sharedVariables = (shared*) sharedStruct;
    
    for (int k = 1; k <= sharedVariables->totalIteration; k++) {
        pthread_mutex_lock(&(sharedVariables->isReady[sharedVariables->tid]));
        for (int i = sharedVariables->startIdx; i < sharedVariables->endIdx; i++) {
            bool diedDueToFighting = false;
            int row = getRow(sharedVariables->nRows, sharedVariables->nCols, i);
            int col = getCol(sharedVariables->nRows, sharedVariables->nCols, i);
            int nextState = getNextState(sharedVariables->world, 
                sharedVariables->inv, sharedVariables->nRows, sharedVariables->nCols, row, col, &diedDueToFighting);

            setValueAt(sharedVariables->wholeNewWorld, sharedVariables->nRows, sharedVariables->nCols, row, col, nextState);

            if (diedDueToFighting)
            {   
                // to check for where is the death toll
                pthread_mutex_lock(sharedVariables->mutex);
                //need synchronisation here
                (*(sharedVariables->deathToll))++;
                pthread_mutex_unlock(sharedVariables->mutex);
            }
        }
        pthread_barrier_wait(sharedVariables->barrier);
    }
}

/**
 * The main simulation logic.
 * 
 * goi does not own startWorld, invasionTimes or invasionPlans and should not modify or attempt to free them.
 * nThreads is the number of threads to simulate with. It is ignored by the sequential implementation.
 */
pthread_barrier_t barrier;

int goi(int nThreads, int nGenerations, const int *startWorld, int nRows, int nCols, int nInvasions, const int *invasionTimes, int **invasionPlans)
{
    // death toll due to fighting
    int deathToll = 0;

    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    
    pthread_t threads[nThreads];
    long threadsId[nThreads];
    pthread_mutex_t isReady[nThreads];
    pthread_barrier_init(&barrier, NULL, nThreads + 1);
    shared** sharedStructs = malloc(sizeof(shared) * nThreads); // need to clean
    if (sharedStructs == NULL) {
        printf("ERROR\n");
        exit(-1);
    }
    int totalGrids = nRows * nCols;
    int threadSize = totalGrids / nThreads;
    int index = 0;

    // init the world!
    // we make a copy because we do not own startWorld (and will perform free() on world)
    int *world = malloc(sizeof(int) * nRows * nCols);
    if (world == NULL)
    {
        return -1;
    }
    for (int row = 0; row < nRows; row++)
    {
        for (int col = 0; col < nCols; col++)
        {
            setValueAt(world, nRows, nCols, row, col, getValueAt(startWorld, nRows, nCols, row, col));
        }
    }

    // initialize the structs, startIdx and endIdx here
    for (int i = 0; i < nThreads - 1; i++) {
        shared* item = malloc(sizeof(shared));
        item->world = world;
        item->mutex = &mutex;
        item->nRows = nRows;
        item->nCols = nCols;
        item->deathToll = &deathToll;
        item->tid = i;
        item->startIdx = index;
        item->endIdx = fmin(index + threadSize, totalGrids);
        item->isReady = isReady;
        pthread_mutex_init(&(isReady[i]), NULL);
        index = index + threadSize;
        sharedStructs[i] = item;
        threadsId[i] = i;
        item->totalIteration = nGenerations;
        item->barrier = &barrier;
        printf("creating thread %d with startIndex: %i and endIdx: %i\n", i, item->startIdx, item->endIdx);
    }

    // the nThreads - 1 thread
    shared* lastItem = malloc(sizeof(shared));
    lastItem->world = world;
    lastItem->mutex = &mutex;
    lastItem->isReady = isReady;
    pthread_mutex_init(&(isReady[nThreads - 1]), NULL);
    lastItem->nRows = nRows;
    lastItem->nCols = nCols;
    lastItem->deathToll = &deathToll;
    lastItem->tid = nThreads - 1;
    lastItem->startIdx = index;
    lastItem->endIdx = totalGrids;
    lastItem->totalIteration = nGenerations;
    lastItem->barrier = &barrier;
    sharedStructs[nThreads - 1] = lastItem;
    threadsId[nThreads - 1] = nThreads - 1;
    printf("creating thread %d with startIndex: %i and endIdx: %i\n", nThreads - 1, lastItem->startIdx, lastItem->endIdx);

#if PRINT_GENERATIONS
    printf("\n=== WORLD 0 ===\n");
    printWorld(world, nRows, nCols);
#endif

#if EXPORT_GENERATIONS
    exportWorld(world, nRows, nCols);
#endif
    bool spawnThreads = false;
    // Begin simulating
    int invasionIndex = 0;
    for (int i = 1; i <= nGenerations; i++)
    {
        // is there an invasion this generation?
        int *inv = NULL;
        if (invasionIndex < nInvasions && i == invasionTimes[invasionIndex])
        {
            // we make a copy because we do not own invasionPlans
            inv = malloc(sizeof(int) * nRows * nCols);
            if (inv == NULL)
            {
                free(world);
                return -1;
            }
            for (int row = 0; row < nRows; row++)
            {
                for (int col = 0; col < nCols; col++)
                {
                    setValueAt(inv, nRows, nCols, row, col, getValueAt(invasionPlans[invasionIndex], nRows, nCols, row, col));
                }
            }
            invasionIndex++;
        }

        // create the next world state

        int *wholeNewWorld = malloc(sizeof(int) * nRows * nCols);
        if (wholeNewWorld == NULL)
        {
            if (inv != NULL)
            {
                free(inv);
            }
            free(world);
            return -1;
        }

        int rc;
        for (int t = 0; t < nThreads; t++) {
            // get the struct
            shared* item = sharedStructs[t];
            item->world = world;
            item->inv = inv;
            item->wholeNewWorld = wholeNewWorld;
            item->iteration = i;
            pthread_mutex_unlock(&(item->isReady[t]));
            if (!spawnThreads) {
                rc = pthread_create(&threads[t], NULL, &subroutine,
                    (void*)item);
                
                if (rc) {
                    printf("Error: Return code from pthread_create() is %d\n", rc);
                    exit(-1);
                }
            }
        }
            
        
        spawnThreads = true;
        pthread_barrier_wait(&barrier);

        if (inv != NULL)
        {
            free(inv);
        }

        // swap worlds
        
        free(world);
        world = wholeNewWorld;

#if PRINT_GENERATIONS
        printf("\n=== WORLD %d ===\n", i);
        printWorld(world, nRows, nCols);
#endif

#if EXPORT_GENERATIONS
        exportWorld(world, nRows, nCols);
#endif
    }

    for (int i = 0; i < nThreads; i++) {
        pthread_join(threads[i], NULL);
    }

    free(world);

    /* clean up the structs*/
    for (int i = 0; i < nThreads; i++) {
        shared* item = sharedStructs[i];
        // free the mutex
        pthread_mutex_destroy(item->mutex);
        free(item);
    }
    
    free(sharedStructs);

    return deathToll;
}
