#include "Halide.h"

using namespace Halide;

class DmaPipeline : public Generator<DmaPipeline> {
public:
    Input<Buffer<uint8_t>> input_y{"input_y", 2};
    Input<Buffer<uint8_t>> input_uv{"input_uv", 3};
    Output<Buffer<uint8_t>> output_y{"output_y", 2};
    Output<Buffer<uint8_t>> output_uv{"output_uv", 3};

    enum class UserOptions { Basic, Fold, Async, Split, Split_Fold };
    GeneratorParam<UserOptions> options{"options",
            /* default value */
             UserOptions::Basic,
            /* map from names to values */
            {{ "none", UserOptions::Basic },
             { "fold", UserOptions::Fold },
             { "async", UserOptions::Async },
             { "split", UserOptions::Split },
             { "split_fold", UserOptions::Split_Fold }}};

    void generate() {
        Var x{"x"}, y{"y"}, c{"c"};

        // We could use 'in' to generate the input copies, but we can't name the variables that way.
        Func input_y_copy("input_y_copy"), input_uv_copy("input_uv_copy");

        Func work_y("work_y");
        Func work_uv("work_uv");

        input_y_copy(x, y) = input_y(x, y);
        work_y(x, y) = input_y_copy(x, y) * 2;
        output_y(x, y) = work_y(x, y);

        input_uv_copy(x, y, c) = input_uv(x, y, c);
        work_uv(x, y, c) = input_uv_copy(x, y, c) * 2;
        output_uv(x, y, c) = work_uv(x, y, c);

        Var tx("tx"), ty("ty");

        // Do some common scheduling here.
        output_y
            .compute_root()
            .copy_to_device();

        output_uv
            .compute_root()
            .copy_to_device()
            .bound(c, 0, 2)
            .reorder(c, x, y);

        // tweak stride/extent to handle UV deinterleaving
        input_uv.dim(0).set_stride(2);
        input_uv.dim(2).set_stride(1).set_bounds(0, 2);
        output_uv.dim(0).set_stride(2);
        output_uv.dim(2).set_stride(1).set_bounds(0, 2);

        // Break the output into tiles.
        const int tile_width = 128;
        const int tile_height = 32;

        switch ((UserOptions)options) {
            case UserOptions::Basic:
            default:
                output_y
                    .tile(x, y, tx, ty, x, y, tile_width, tile_height, TailStrategy::RoundUp);

                output_uv
                    .tile(x, y, tx, ty, x, y, tile_width, tile_height, TailStrategy::RoundUp);

                input_y_copy
                    .compute_at(output_y, tx)
                    .copy_to_host();

                input_uv_copy
                    .compute_at(output_uv, tx)
                    .copy_to_host()
                    .bound(c, 0, 2)
                    .reorder_storage(c, x, y);
            break;
            case UserOptions::Fold:
                output_y
                    .tile(x, y, tx, ty, x, y, tile_width, tile_height, TailStrategy::RoundUp);

                output_uv
                    .reorder(c, x, y)   // to handle UV interleave, with 'c' inner most loop, as DMA'd into buffer
                    .tile(x, y, tx, ty, x, y, tile_width, tile_height, TailStrategy::RoundUp);

                input_y_copy
                    .copy_to_host()
                    .compute_at(output_y, tx)
                    .store_at(output_y, ty)
                    .fold_storage(x, tile_width * 2);

                input_uv_copy
                    .copy_to_host()
                    .compute_at(output_uv, tx)
                    .store_at(output_uv, ty)
                    .reorder_storage(c, x, y)
                    .fold_storage(x, tile_width * 2);

            break;
            case UserOptions::Async:
                output_y
                    .tile(x, y, tx, ty, x, y, tile_width, tile_height, TailStrategy::RoundUp);

                output_uv
                    .tile(x, y, tx, ty, x, y, tile_width, tile_height, TailStrategy::RoundUp);

                input_y_copy
                    .copy_to_host()
                    .async()
                    .compute_at(output_y, tx)
                    .store_at(output_y, ty)
                    .fold_storage(x, tile_width * 2);

                input_uv_copy
                    .copy_to_host()
                    .async()
                    .compute_at(output_uv, tx)
                    .store_at(output_uv, ty)
                    .reorder_storage(c, x, y)
                    .fold_storage(x, tile_width * 2);
            break;
            case UserOptions::Split: {
                Var yo, yi;

                Expr fac_y = output_y.dim(1).extent()/2;
                output_y
                    .split(y, yo, yi, fac_y)
                    .tile(x, yi, tx, ty, x, y, tile_width, tile_height, TailStrategy::RoundUp)
                    .parallel(yo);

                Expr fac_uv = output_uv.dim(1).extent()/2;
                output_uv
                    .split(y, yo, yi, fac_uv)
                    .tile(x, yi, tx, ty, x, y, tile_width, tile_height, TailStrategy::RoundUp)
                    .parallel(yo);

                input_y_copy
                    .copy_to_host()
                    .compute_at(output_y, tx);

                input_uv_copy
                    .copy_to_host()
                    .compute_at(output_uv, tx)
                    .bound(c, 0, 2)
                    .reorder_storage(c, x, y);
            }
            break;
            case UserOptions::Split_Fold: {
                Var yo, yi;

                Expr fac_y = output_y.dim(1).extent()/2;
                output_y
                    .split(y, yo, yi, fac_y)
                    .tile(x, yi, tx, ty, x, y, tile_width, tile_height, TailStrategy::RoundUp)
                    .parallel(yo);

                Expr fac_uv = output_uv.dim(1).extent()/2;
                output_uv
                    .split(y, yo, yi, fac_uv)
                    .tile(x, yi, tx, ty, x, y, tile_width, tile_height, TailStrategy::RoundUp)
                    .parallel(yo);

                input_y_copy
                    .copy_to_host()
                    .compute_at(output_y, tx)
                    .store_at(output_y, ty)
                    .async()
                    .fold_storage(x, tile_width * 2);

                input_uv_copy
                    .copy_to_host()
                    .compute_at(output_uv, tx)
                    .store_at(output_uv, ty)
                    .async()
                    .bound(c, 0, 2)
                    .reorder_storage(c, x, y)
                    .fold_storage(x, tile_width * 2);
            }
            break;
        }

        // Schedule the work in tiles (same for all DMA schedules).
        work_y.compute_at(output_y, tx);

        work_uv
            .compute_at(output_uv, tx)
            .bound(c, 0, 2)
            .reorder_storage(c, x, y);
    }
};

HALIDE_REGISTER_GENERATOR(DmaPipeline, pipeline_nv12_linear_rw_basic)
