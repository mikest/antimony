#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

#include "tree/eval.h"
#include "tree/tree.h"
#include "tree/render.h"

#include "util/switches.h"

/*  region8
 *
 *  Renders a tree pixel-by-pixel into a given region,
 *  using the eval_r function to find an array of results in
 *  a single pass through the tree.
 *
 */
_STATIC_
void region8(MathTree* tree, Region region, uint8_t** img);

/*  region16
 *
 *  Renders a tree pixel-by-pixel into a given region,
 *  using the eval_r function to find an array of results in
 *  a single pass through the tree.
 *
 */
_STATIC_
void region16(MathTree* tree, Region region, uint16_t** img);

////////////////////////////////////////////////////////////////////////////////
void render8(MathTree* tree, Region region,
             uint8_t** img, volatile int* halt,
             void (*callback)())
{
    // Special interrupt system, set asynchronously by on high
    if (*halt)  return;

    // Render pixel-by-pixel if we're below a certain size.
    if (region.voxels > 0 && region.voxels < MIN_VOLUME) {
        if (callback)   (*callback)();
        region8(tree, region, img);
        return;
    }


    // Pre-emptively halt evaluation if all the points in this
    // region are already light.
    uint8_t L = region.L[region.nk] >> 8;
    bool cull = true;
    for (int row = region.jmin; cull && row < region.jmin + region.nj; ++row) {
        for (int col = region.imin; cull && col < region.imin + region.ni; ++col) {
            if (L > img[row][col]) {
                cull = false;
                break;
            }
        }
    }
    if (cull) return;

    Interval X = {region.X[0], region.X[region.ni]},
             Y = {region.Y[0], region.Y[region.nj]},
             Z = {region.Z[0], region.Z[region.nk]};

    Interval result = eval_i(tree, X, Y, Z);

    // If we're inside the object, fill with color.
    if (result.upper < 0) {
        for (int row = region.jmin; row < region.jmin + region.nj; ++row) {
            for (int col = region.imin; col < region.imin + region.ni; ++col) {
                if (L > img[row][col])  img[row][col] = L;
            }
        }
    }

    // In unambiguous cases, return immediately
    if (result.upper < 0 || result.lower >= 0)  return;

#if PRUNE
    disable_nodes(tree);
    disable_nodes_binary(tree);
#endif

    // Subdivide and recurse if we're not at voxel size.
    if (region.ni*region.nj*region.nk > 1) {
        Region A, B;

        bisect(region, &A, &B);

        render8(tree, B, img, halt, callback);
        render8(tree, A, img, halt, callback);
    }

#if PRUNE
    enable_nodes(tree);
#endif

}

////////////////////////////////////////////////////////////////////////////////

_STATIC_
void region8(MathTree* tree, Region region, uint8_t** img)
{
    float *X = malloc(region.voxels*sizeof(float)),
          *Y = malloc(region.voxels*sizeof(float)),
          *Z = malloc(region.voxels*sizeof(float));

    // Copy the X, Y, Z vectors into a flattened matrix form.
    int q = 0;
    for (int k = region.nk - 1; k >= 0; --k) {
        for (int j = 0; j < region.nj; ++j) {
            for (int i = 0; i < region.ni; ++i) {
                X[q] = region.X[i];
                Y[q] = region.Y[j];
                Z[q] = region.Z[k];
                q++;
            }
        }
    }
    region.X = X;
    region.Y = Y;
    region.Z = Z;

    float* result = eval_r(tree, region);

    // Free the allocated matrices
    free(X);
    free(Y);
    free(Z);

    for (int k = region.nk - 1; k >= 0; --k) {
        uint8_t L = region.L[k+1] >> 8;

        for (int j = 0; j < region.nj; ++j) {
            int row = j + region.jmin;

            for (int i = 0; i < region.ni; ++i) {
                int col = i + region.imin;

               if (*(result++) < 0 && img[row][col] < L) {
                    img[row][col] = L;
                }
            }
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

void get_normals8(MathTree* tree,
                  float* restrict X, float* restrict Y, float* restrict Z,
                  unsigned count, float epsilon,
                  float (*normals)[3])
{
    Region dummy;
    dummy.X = X;
    dummy.Y = Y;
    dummy.Z = Z;
    dummy.voxels = count;

    float* base = malloc(count*sizeof(float));
    float* dx = malloc(count*sizeof(float));
    float* dy = malloc(count*sizeof(float));
    float* dz = malloc(count*sizeof(float));

    float* result;
    result = eval_r(tree, dummy);
    memmove(base, result, count*sizeof(float));

    for (int i=0; i < count; ++i)   X[i] += epsilon;
    result = eval_r(tree, dummy);
    memmove(dx, result, count*sizeof(float));
    for (int i=0; i < count; ++i)   X[i] -= epsilon;

    for (int i=0; i < count; ++i)   Y[i] += epsilon;
    result = eval_r(tree, dummy);
    memmove(dy, result, count*sizeof(float));
    for (int i=0; i < count; ++i)   Y[i] -= epsilon;

    for (int i=0; i < count; ++i)   Z[i] += epsilon;
    result = eval_r(tree, dummy);
    memmove(dz, result, count*sizeof(float));

    // Calculate normals and copy over.
    for (int i=0; i < count; ++i)
    {
        float x = dx[i] - base[i];
        float y = dy[i] - base[i];
        float z = dz[i] - base[i];

        float dist = sqrt(pow(x,2) + pow(y, 2) + pow(z,2));
        normals[i][0] = dist ? x/dist : 0;
        normals[i][1] = dist ? y/dist : 0;
        normals[i][2] = dist ? z/dist : 0;
    }
    free(base);
    free(dx);
    free(dy);
    free(dz);
}

_STATIC_
void shade_pixels8(unsigned count, float (*normals)[3],
                   unsigned* is, unsigned* js, uint8_t (**out)[3])
{
    for (int a=0; a < count; ++a)
    {
        for (int b=0; b < 3; ++b)
        {
            out[js[a]][is[a]][b] = fabs(normals[a][b]) * 255;
        }
    }
}

void shaded8(struct MathTree_ *tree, Region region, uint8_t **depth,
             uint8_t (**out)[3], volatile int *halt,
             void (*callback)())
{
    float *X = malloc(MIN_VOLUME*sizeof(float)),
          *Y = malloc(MIN_VOLUME*sizeof(float)),
          *Z = malloc(MIN_VOLUME*sizeof(float));

    unsigned *is = malloc(MIN_VOLUME*sizeof(unsigned));
    unsigned *js = malloc(MIN_VOLUME*sizeof(unsigned));

    float (*normals)[3] = malloc(MIN_VOLUME*sizeof(float[3]));

    const float epsilon = (region.X[1] - region.X[0]) / 10.0f;

    unsigned count = 0;
    for (unsigned j=0; j < region.nj && !*halt; ++j)
    {
        if (callback)   (*callback)();

        for (unsigned i=0; i < region.ni && !*halt; ++i)
        {
            // Load this pixel into the set of pixels to render
            if (depth[j][i])
            {
                X[count] = region.X[i];
                Y[count] = region.Y[j];
                Z[count] = region.Z[0] + depth[j][i] / 255.0f *
                            (region.Z[region.nk] - region.Z[0]);

                is[count] = i;
                js[count] = j;
                count++;
            }

            if (count == MIN_VOLUME ||
                    (count && j == region.nj - 1 && i == region.ni - 1))
            {
                get_normals8(tree, X, Y, Z, count, epsilon, normals);
                shade_pixels8(count, normals, is, js, out);
                count = 0;
            }
        }
    }

    free(X);
    free(Y);
    free(Z);

    free(is);
    free(js);

    free(normals);
}


////////////////////////////////////////////////////////////////////////////////
void render16(MathTree* tree, Region region,
              uint16_t** img, volatile int* halt)
{
    if (tree == NULL)  return;

    // Special interrupt system, set asynchronously by on high
    if (*halt)  return;

    // Render pixel-by-pixel if we're below a certain size.
    if (region.voxels > 0 && region.voxels < MIN_VOLUME) {
        region16(tree, region, img);
        return;
    }

    // Pre-emptively halt evaluation if all the points in this
    // region are already light.
    uint16_t L = region.L[region.nk];
    bool cull = true;
    for (int row = region.jmin; cull && row < region.jmin + region.nj; ++row) {
        for (int col = region.imin; cull && col < region.imin + region.ni; ++col) {
            if (L > img[row][col]) {
                cull = false;
                break;
            }
        }
    }
    if (cull) return;

    Interval X = {region.X[0], region.X[region.ni]},
             Y = {region.Y[0], region.Y[region.nj]},
             Z = {region.Z[0], region.Z[region.nk]};

    Interval result = eval_i(tree, X, Y, Z);

    // If we're inside the object, fill with color.
    if (result.upper < 0) {
        for (int row = region.jmin; row < region.jmin + region.nj; ++row) {
            for (int col = region.imin; col < region.imin + region.ni; ++col) {
                if (L > img[row][col])  img[row][col] = L;
            }
        }
    }

    // In unambiguous cases, return immediately
    if (result.upper < 0 || result.lower >= 0)  return;

#if PRUNE
    disable_nodes(tree);
    disable_nodes_binary(tree);
#endif

    // Subdivide and recurse if we're not at voxel size.
    if (region.ni*region.nj*region.nk > 1) {
        Region A, B;
        bisect(region, &A, &B);

        render16(tree, B, img, halt);
        render16(tree, A, img, halt);
    }

#if PRUNE
    enable_nodes(tree);
#endif

}

////////////////////////////////////////////////////////////////////////////////

_STATIC_
void region16(MathTree* tree, Region region, uint16_t** img)
{
    float *X = malloc(region.voxels*sizeof(float)),
          *Y = malloc(region.voxels*sizeof(float)),
          *Z = malloc(region.voxels*sizeof(float));

    // Copy the X, Y, Z vectors into a flattened matrix form.
    int q = 0;
    for (int k = region.nk - 1; k >= 0; --k) {
        for (int j = 0; j < region.nj; ++j) {
            for (int i = 0; i < region.ni; ++i) {
                X[q] = region.X[i];
                Y[q] = region.Y[j];
                Z[q] = region.Z[k];
                q++;
            }
        }
    }
    region.X = X;
    region.Y = Y;
    region.Z = Z;

    float* result = eval_r(tree, region);

    // Free the allocated matrices
    free(X);
    free(Y);
    free(Z);

    for (int k = region.nk - 1; k >= 0; --k) {
        uint16_t L = region.L[k+1];

        for (int j = 0; j < region.nj; ++j) {
            int row = j + region.jmin;

            for (int i = 0; i < region.ni; ++i) {
                int col = i + region.imin;

               if (*(result++) < 0 && img[row][col] < L) {
                    img[row][col] = L;
                }
            }
        }
    }
}
