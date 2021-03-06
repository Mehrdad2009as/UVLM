#pragma once

#include "types.h"
#include "constants.h"
#include "triads.h"
#include "mapping.h"
#include "geometry.h"
#include "biotsavart.h"
#include "matrix.h"
#include "wake.h"
#include "postproc.h"
#include "linear_solver.h"

#include <iostream>

// DECLARATIONS
namespace UVLM
{
    namespace Steady
    {
        template <typename t_zeta,
                  typename t_zeta_dot,
                  typename t_uext,
                  typename t_zeta_star,
                  typename t_gamma,
                  typename t_gamma_star,
                  typename t_forces>
        void solver
        (
            t_zeta& zeta,
            t_zeta_dot& zeta_dot,
            t_uext& uext,
            t_zeta_star& zeta_star,
            t_gamma& gamma,
            t_gamma_star& gamma_star,
            t_forces& forces,
            const UVLM::Types::VMopts& options,
            const UVLM::Types::FlightConditions& flightconditions
        );

        template <typename t_zeta,
                  typename t_zeta_col,
                  typename t_uext_col,
                  typename t_zeta_star,
                  typename t_gamma,
                  typename t_gamma_star,
                  typename t_normals>
        void solve_horseshoe
        (
            t_zeta& zeta,
            t_zeta_col& zeta_col,
            t_uext_col& uext_col,
            t_zeta_star& zeta_star,
            t_gamma& gamma,
            t_gamma_star& gamma_star,
            t_normals& normals,
            const UVLM::Types::VMopts& options,
            const UVLM::Types::FlightConditions& flightconditions
        );
        template <typename t_zeta,
                  typename t_zeta_col,
                  typename t_uext_col,
                  typename t_zeta_star,
                  typename t_gamma,
                  typename t_gamma_star,
                  typename t_normals>
        void solve_discretised
        (
            t_zeta& zeta,
            t_zeta_col& zeta_col,
            t_uext_col& uext_col,
            t_zeta_star& zeta_star,
            t_gamma& gamma,
            t_gamma_star& gamma_star,
            t_normals& normals,
            const UVLM::Types::VMopts& options,
            const UVLM::Types::FlightConditions& flightconditions
        );
    }
}

/*-----------------------------------------------------------------------------

-----------------------------------------------------------------------------*/
template <typename t_zeta,
          typename t_zeta_dot,
          typename t_uext,
          typename t_zeta_star,
          typename t_gamma,
          typename t_gamma_star,
          typename t_forces>
void UVLM::Steady::solver
(
    t_zeta& zeta,
    t_zeta_dot& zeta_dot,
    t_uext& uext,
    t_zeta_star& zeta_star,
    t_gamma& gamma,
    t_gamma_star& gamma_star,
    t_forces& forces,
    const UVLM::Types::VMopts& options,
    const UVLM::Types::FlightConditions& flightconditions
)
{
    // Generate collocation points info
    //  Declaration
    UVLM::Types::VecVecMatrixX zeta_col;
    UVLM::Types::VecVecMatrixX uext_col;

    //  Allocation and mapping
    UVLM::Geometry::generate_colocationMesh(zeta, zeta_col);
    UVLM::Geometry::generate_colocationMesh(uext, uext_col);

    // panel normals
    UVLM::Types::VecVecMatrixX normals;
    UVLM::Types::allocate_VecVecMat(normals, zeta_col);
    UVLM::Geometry::generate_surfaceNormal(zeta, normals);

    // solve the steady horseshoe problem
    UVLM::Steady::solve_horseshoe
    (
        zeta,
        zeta_col,
        uext_col,
        zeta_star,
        gamma,
        gamma_star,
        normals,
        options,
        flightconditions
    );

    // if options.horseshoe, it is finished.
    if (options.horseshoe)
    {
        UVLM::PostProc::calculate_static_forces
        (
            zeta,
            zeta_star,
            gamma,
            gamma_star,
            uext,
            forces,
            options,
            flightconditions
        );
        return;
    }

    // if not, the wake has to be transformed into a discretised, non-horseshoe
    // one:
    UVLM::Types::Vector3 u_steady;
    u_steady << uext[0][0](0,0),
                uext[0][1](0,0),
                uext[0][2](0,0);
    double delta_x = u_steady.norm()*options.dt;

    UVLM::Wake::Horseshoe::to_discretised(zeta_star,
                                          gamma_star,
                                          delta_x);

    double zeta_star_norm_first = 0.0;
    double zeta_star_norm_previous = 0.0;
    double zeta_star_norm = 0.0;

    UVLM::Types::VecVecMatrixX zeta_star_previous;
    if (options.n_rollup != 0)
    {
        zeta_star_norm_first = UVLM::Types::norm_VecVec_mat(zeta_star);
        zeta_star_norm_previous = zeta_star_norm_first;
        zeta_star_norm = 0.0;

        UVLM::Types::allocate_VecVecMat(zeta_star_previous, zeta_star);
        UVLM::Types::copy_VecVecMat(zeta_star, zeta_star_previous);
    }

    // ROLLUP LOOP--------------------------------------------------------
    for (uint i_rollup=0; i_rollup<options.n_rollup; ++i_rollup)
    {
        // determine convection velocity u_ind
        UVLM::Types::VecVecMatrixX u_ind;
        UVLM::Types::allocate_VecVecMat(u_ind,
                                        zeta_star);
        // induced velocity by vortex rings
        UVLM::BiotSavart::total_induced_velocity_on_wake(
            zeta,
            zeta_star,
            gamma,
            gamma_star,
            u_ind);
        // convection velocity of the background flow
        for (uint i_surf=0; i_surf<zeta.size(); ++i_surf)
        {
            for (uint i_dim=0; i_dim<UVLM::Constants::NDIM; ++i_dim)
            {
                u_ind[i_surf][i_dim].array() += u_steady(i_dim);
            }
        }

        // convect based on u_ind for all the grid.
        UVLM::Wake::Discretised::convect(zeta_star,
                                         u_ind,
                                         options.dt);
        // move wake 1 row down and discard last row (far field)
        UVLM::Wake::General::displace_VecVecMat(zeta_star);
        UVLM::Wake::General::displace_VecMat(gamma_star);
        // copy trailing edge of zeta into 1st row of zeta_star
        for (uint i_surf=0; i_surf<zeta.size(); ++i_surf)
        {
            for (uint i_dim=0; i_dim<UVLM::Constants::NDIM; ++i_dim)
            {
                zeta_star[i_surf][i_dim].template topRows<1>() =
                    zeta[i_surf][i_dim].template bottomRows<1>();
            }
        }

        // generate AIC again
        if (i_rollup%options.rollup_aic_refresh == 0)
        {
            UVLM::Steady::solve_discretised
            (
                zeta,
                zeta_col,
                uext_col,
                zeta_star,
                gamma,
                gamma_star,
                normals,
                options,
                flightconditions
            );
        }

        // convergence check -------------------
        zeta_star_norm = UVLM::Types::norm_VecVec_mat(zeta_star);
        if (i_rollup != 0)
        {
            // double eps = std::abs((zeta_star_norm - zeta_star_norm_previous)
            //                       /zeta_star_norm_first);
            double eps = std::abs(UVLM::Types::norm_VecVec_mat(zeta_star - zeta_star_previous))/zeta_star_norm_first;
            // std::cout << i_rollup << ", " << eps << std::endl;
            if (eps < options.rollup_tolerance)
            {
                // std::cout << "converged" << std::endl;
                break;
            }
            zeta_star_norm_previous = zeta_star_norm;
            UVLM::Types::copy_VecVecMat(zeta_star, zeta_star_previous);
        }
    }

    UVLM::PostProc::calculate_static_forces
    (
        zeta,
        zeta_star,
        gamma,
        gamma_star,
        uext,
        forces,
        options,
        flightconditions
    );
}



/*-----------------------------------------------------------------------------

-----------------------------------------------------------------------------*/
template <typename t_zeta,
          typename t_zeta_col,
          typename t_uext_col,
          typename t_zeta_star,
          typename t_gamma,
          typename t_gamma_star,
          typename t_normals>
void UVLM::Steady::solve_horseshoe
(
    t_zeta& zeta,
    t_zeta_col& zeta_col,
    t_uext_col& uext_col,
    t_zeta_star& zeta_star,
    t_gamma& gamma,
    t_gamma_star& gamma_star,
    t_normals& normals,
    const UVLM::Types::VMopts& options,
    const UVLM::Types::FlightConditions& flightconditions
)
{
    // wake generation for horseshoe initialisation
    UVLM::Wake::Horseshoe::init(zeta,
                                zeta_star,
                                flightconditions);

    const uint n_surf = options.NumSurfaces;
    // size of rhs
    uint ii = 0;
    for (uint i_surf=0; i_surf<n_surf; ++i_surf)
    {
        uint M = uext_col[i_surf][0].rows();
        uint N = uext_col[i_surf][0].cols();

        ii += M*N;
    }

    const uint Ktotal = ii;
    // RHS generation
    UVLM::Types::VectorX rhs;
    UVLM::Matrix::RHS(zeta_col,
                      zeta_star,
                      uext_col,
                      gamma_star,
                      normals,
                      options,
                      rhs,
                      Ktotal);

    // AIC generation
    UVLM::Types::MatrixX aic = UVLM::Types::MatrixX::Zero(Ktotal, Ktotal);
    UVLM::Matrix::AIC(Ktotal,
                      zeta,
                      zeta_col,
                      zeta_star,
                      uext_col,
                      normals,
                      options,
                      true,
                      aic);

    UVLM::Types::VectorX gamma_flat;
    // gamma_flat = aic.partialPivLu().solve(rhs);
    UVLM::LinearSolver::solve_system
    (
        aic,
        rhs,
        options,
        gamma_flat
    );

    // probably could be done better with a Map
    UVLM::Matrix::reconstruct_gamma(gamma_flat,
                                    gamma,
                                    zeta_col,
                                    zeta_star,
                                    options);

    // copy gamma from trailing edge to wake if steady solution
    UVLM::Wake::Horseshoe::circulation_transfer(gamma,
                                                gamma_star);

}



/*-----------------------------------------------------------------------------

-----------------------------------------------------------------------------*/
template <typename t_zeta,
          typename t_zeta_col,
          typename t_uext_col,
          typename t_zeta_star,
          typename t_gamma,
          typename t_gamma_star,
          typename t_normals>
void UVLM::Steady::solve_discretised
(
    t_zeta& zeta,
    t_zeta_col& zeta_col,
    t_uext_col& uext_col,
    t_zeta_star& zeta_star,
    t_gamma& gamma,
    t_gamma_star& gamma_star,
    t_normals& normals,
    const UVLM::Types::VMopts& options,
    const UVLM::Types::FlightConditions& flightconditions
)
{
    const uint n_surf = options.NumSurfaces;
    // size of rhs
    uint ii = 0;
    for (uint i_surf=0; i_surf<n_surf; ++i_surf)
    {
        uint M = uext_col[i_surf][0].rows();
        uint N = uext_col[i_surf][0].cols();

        ii += M*N;
    }
    const uint Ktotal = ii;

    UVLM::Types::VectorX rhs;
    UVLM::Types::MatrixX aic = UVLM::Types::MatrixX::Zero(Ktotal, Ktotal);
    // #pragma omp parallel sections
    {
        // #pragma omp section
        {
            // RHS generation
            UVLM::Matrix::RHS(zeta_col,
                              zeta_star,
                              uext_col,
                              gamma_star,
                              normals,
                              options,
                              rhs,
                              Ktotal);
        }
        // #pragma omp section
        {
            // AIC generation
            UVLM::Matrix::AIC(Ktotal,
                              zeta,
                              zeta_col,
                              zeta_star,
                              uext_col,
                              normals,
                              options,
                              false,
                              aic);
        }
    }
    UVLM::Types::VectorX gamma_flat;
    // std::cout << aic << std::endl;
    // gamma_flat = aic.partialPivLu().solve(rhs);
    UVLM::LinearSolver::solve_system
    (
        aic,
        rhs,
        options,
        gamma_flat
    );

    // probably could be done better with a Map
    UVLM::Matrix::reconstruct_gamma(gamma_flat,
                                    gamma,
                                    zeta_col,
                                    zeta_star,
                                    options);

    // copy gamma from trailing edge to wake
    int in_n_rows = -1;
    if (!options.Steady) {in_n_rows = 1;}
    UVLM::Wake::Horseshoe::circulation_transfer(gamma,
                                                gamma_star,
                                                in_n_rows);

}
