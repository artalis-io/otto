/*
 * sh_eov.c - Hungarian HD72/EOV ↔ WGS84 Coordinate Conversion
 *
 * Uses a 7-parameter Bursa-Wolf (Helmert) datum transformation
 * between HD72 (Hungarian Datum 1972) and WGS84, combined with
 * transverse Mercator projection for EOV.
 *
 * EOV projection parameters:
 *   - Central meridian: 19.048571778° E
 *   - Scale factor: 0.99993
 *   - False easting: 650000 m
 *   - False northing: 200000 m
 *   - GRS67 ellipsoid: a = 6378160 m, f = 1/298.247167427
 *
 * HD72 → WGS84 Bursa-Wolf parameters (EPSG:1448):
 *   dX = +52.684, dY = -71.194, dZ = -13.975
 *   rX = +0.312, rY = +0.1063, rZ = +0.3729 (arc-seconds)
 *   dS = +1.0191 (ppm)
 */

#include "sh_eov.h"
#include <math.h>

/* GRS67 ellipsoid (used by HD72/EOV) */
#define GRS67_A  6378160.0
#define GRS67_F  (1.0 / 298.247167427)
#define GRS67_B  (GRS67_A * (1.0 - GRS67_F))
#define GRS67_E2 (2.0 * GRS67_F - GRS67_F * GRS67_F)

/* WGS84 ellipsoid */
#define WGS84_A  6378137.0
#define WGS84_F  (1.0 / 298.257223563)
#define WGS84_B  (WGS84_A * (1.0 - WGS84_F))
#define WGS84_E2 (2.0 * WGS84_F - WGS84_F * WGS84_F)

/* EOV projection parameters (EPSG:23700) */
#define EOV_PHI0   (47.14439372222222 * M_PI / 180.0) /* Lat of origin: 47°08'39.8174"N */
#define EOV_LAM0   (19.048571778 * M_PI / 180.0) /* Central meridian in radians */
#define EOV_K0     0.99993                         /* Scale factor */
#define EOV_FE     650000.0                        /* False easting */
#define EOV_FN     200000.0                        /* False northing */

/* Bursa-Wolf parameters HD72 → WGS84 (EPSG:1448) */
#define BW_DX     52.684
#define BW_DY    -71.194
#define BW_DZ    -13.975
#define BW_RX    (0.312    * M_PI / (180.0 * 3600.0)) /* arc-sec to radians */
#define BW_RY    (0.1063   * M_PI / (180.0 * 3600.0))
#define BW_RZ    (0.3729   * M_PI / (180.0 * 3600.0))
#define BW_DS    (1.0191e-6)  /* ppm to ratio */

#define DEG2RAD (M_PI / 180.0)
#define RAD2DEG (180.0 / M_PI)

/* Meridian arc length from equator to latitude phi on ellipsoid (a, e2) */
static double meridian_arc(double phi, double a, double e2)
{
    double e4 = e2 * e2;
    double e6 = e4 * e2;
    double A0 = 1.0 - e2 / 4.0 - 3.0 * e4 / 64.0 - 5.0 * e6 / 256.0;
    double A2 = 3.0 / 8.0 * (e2 + e4 / 4.0 + 15.0 * e6 / 128.0);
    double A4 = 15.0 / 256.0 * (e4 + 3.0 * e6 / 4.0);
    double A6 = 35.0 * e6 / 3072.0;
    return a * (A0 * phi - A2 * sin(2.0 * phi) + A4 * sin(4.0 * phi) - A6 * sin(6.0 * phi));
}

/* Geodetic (lat, lon) → EOV (Y=easting, X=northing) on GRS67 */
static void geo_to_eov(double phi, double lam, double *eov_y, double *eov_x)
{
    double sp = sin(phi), cp = cos(phi), tp = tan(phi);
    double N = GRS67_A / sqrt(1.0 - GRS67_E2 * sp * sp);
    double T = tp * tp;
    double C = GRS67_E2 * cp * cp / (1.0 - GRS67_E2);
    double A = (lam - EOV_LAM0) * cp;
    double M = meridian_arc(phi, GRS67_A, GRS67_E2);
    double M0 = meridian_arc(EOV_PHI0, GRS67_A, GRS67_E2);

    double A2 = A * A;
    double A4 = A2 * A2;

    *eov_x = EOV_FN + EOV_K0 * (M - M0 + N * tp * (A2 / 2.0 +
             (5.0 - T + 9.0 * C + 4.0 * C * C) * A4 / 24.0 +
             (61.0 - 58.0 * T + T * T) * A4 * A2 / 720.0));

    *eov_y = EOV_FE + EOV_K0 * N * (A +
             (1.0 - T + C) * A * A2 / 6.0 +
             (5.0 - 18.0 * T + T * T) * A * A4 / 120.0);
}

/* EOV (Y=easting, X=northing) → geodetic (lat, lon) on GRS67 */
static void eov_to_geo(double eov_y, double eov_x, double *phi, double *lam)
{
    double M0 = meridian_arc(EOV_PHI0, GRS67_A, GRS67_E2);
    double M = M0 + (eov_x - EOV_FN) / EOV_K0;

    /* Footpoint latitude via iterative inversion of meridian arc */
    double mu = M / (GRS67_A * (1.0 - GRS67_E2 / 4.0 - 3.0 * GRS67_E2 * GRS67_E2 / 64.0
                 - 5.0 * GRS67_E2 * GRS67_E2 * GRS67_E2 / 256.0));
    double e1 = (1.0 - sqrt(1.0 - GRS67_E2)) / (1.0 + sqrt(1.0 - GRS67_E2));
    double e12 = e1 * e1;
    double phi1 = mu
        + (3.0 * e1 / 2.0 - 27.0 * e12 * e1 / 32.0) * sin(2.0 * mu)
        + (21.0 * e12 / 16.0 - 55.0 * e12 * e12 / 32.0) * sin(4.0 * mu)
        + (151.0 * e12 * e1 / 96.0) * sin(6.0 * mu);

    double sp1 = sin(phi1), cp1 = cos(phi1), tp1 = tan(phi1);
    double N1 = GRS67_A / sqrt(1.0 - GRS67_E2 * sp1 * sp1);
    double R1 = GRS67_A * (1.0 - GRS67_E2) / pow(1.0 - GRS67_E2 * sp1 * sp1, 1.5);
    double T1 = tp1 * tp1;
    double C1 = GRS67_E2 * cp1 * cp1 / (1.0 - GRS67_E2);
    double D = (eov_y - EOV_FE) / (N1 * EOV_K0);
    double D2 = D * D;
    double D4 = D2 * D2;

    *phi = phi1 - (N1 * tp1 / R1) * (D2 / 2.0
           - (5.0 + 3.0 * T1 + 10.0 * C1 - 4.0 * C1 * C1 - 9.0 * GRS67_E2 / (1.0 - GRS67_E2)) * D4 / 24.0
           + (61.0 + 90.0 * T1 + 45.0 * T1 * T1) * D4 * D2 / 720.0);

    *lam = EOV_LAM0 + (D - (1.0 + 2.0 * T1 + C1) * D * D2 / 6.0
           + (5.0 - 2.0 * C1 + 28.0 * T1 - 3.0 * C1 * C1) * D * D4 / 120.0) / cp1;
}

/* Geodetic to cartesian (ECEF) */
static void geo_to_xyz(double phi, double lam, double h,
                       double a, double e2,
                       double *X, double *Y, double *Z)
{
    double sp = sin(phi), cp = cos(phi);
    double N = a / sqrt(1.0 - e2 * sp * sp);
    *X = (N + h) * cp * cos(lam);
    *Y = (N + h) * cp * sin(lam);
    *Z = (N * (1.0 - e2) + h) * sp;
}

/* Cartesian (ECEF) to geodetic */
static void xyz_to_geo(double X, double Y, double Z,
                       double a, double e2,
                       double *phi, double *lam, double *h)
{
    *lam = atan2(Y, X);
    double p = sqrt(X * X + Y * Y);
    double b = a * sqrt(1.0 - e2);
    double ep2 = (a * a - b * b) / (b * b);
    double theta = atan2(Z * a, p * b);
    double st = sin(theta), ct = cos(theta);
    *phi = atan2(Z + ep2 * b * st * st * st,
                 p - e2 * a * ct * ct * ct);
    double sp = sin(*phi);
    double N = a / sqrt(1.0 - e2 * sp * sp);
    double cp = cos(*phi);
    if (fabs(cp) > 1e-10)
        *h = p / cp - N;
    else
        *h = fabs(Z) - b;
}

/* Apply 7-parameter Bursa-Wolf transformation (HD72 → WGS84) */
static void bursa_wolf_hd72_to_wgs84(double X_in, double Y_in, double Z_in,
                                      double *X_out, double *Y_out, double *Z_out)
{
    double s = 1.0 + BW_DS;
    *X_out = BW_DX + s * (X_in - BW_RZ * Y_in + BW_RY * Z_in);
    *Y_out = BW_DY + s * (BW_RZ * X_in + Y_in - BW_RX * Z_in);
    *Z_out = BW_DZ + s * (-BW_RY * X_in + BW_RX * Y_in + Z_in);
}

/* Apply inverse 7-parameter Bursa-Wolf transformation (WGS84 → HD72) */
static void bursa_wolf_wgs84_to_hd72(double X_in, double Y_in, double Z_in,
                                      double *X_out, double *Y_out, double *Z_out)
{
    double s = 1.0 + BW_DS;
    /* Inverse: use negative translations and rotations, 1/s scale */
    double si = 1.0 / s;
    double dx = X_in - BW_DX;
    double dy = Y_in - BW_DY;
    double dz = Z_in - BW_DZ;
    *X_out = si * (dx + BW_RZ * dy - BW_RY * dz);
    *Y_out = si * (-BW_RZ * dx + dy + BW_RX * dz);
    *Z_out = si * (BW_RY * dx - BW_RX * dy + dz);
}

int sh_eov_to_wgs84(double eov_y, double eov_x, double *lat, double *lon)
{
    if (!lat || !lon) return -1;

    /* Basic range check for EOV coordinates (Hungary) */
    if (eov_y < 400000 || eov_y > 950000 || eov_x < 30000 || eov_x > 370000)
        return -1;

    /* Step 1: EOV → geodetic on GRS67 (HD72 datum) */
    double phi_hd72, lam_hd72;
    eov_to_geo(eov_y, eov_x, &phi_hd72, &lam_hd72);

    /* Step 2: Geodetic GRS67 → cartesian (ECEF) on GRS67 */
    double X_hd72, Y_hd72, Z_hd72;
    geo_to_xyz(phi_hd72, lam_hd72, 0.0, GRS67_A, GRS67_E2,
               &X_hd72, &Y_hd72, &Z_hd72);

    /* Step 3: Bursa-Wolf HD72 → WGS84 in cartesian */
    double X_wgs, Y_wgs, Z_wgs;
    bursa_wolf_hd72_to_wgs84(X_hd72, Y_hd72, Z_hd72, &X_wgs, &Y_wgs, &Z_wgs);

    /* Step 4: Cartesian WGS84 → geodetic WGS84 */
    double h;
    double phi_wgs, lam_wgs;
    xyz_to_geo(X_wgs, Y_wgs, Z_wgs, WGS84_A, WGS84_E2,
               &phi_wgs, &lam_wgs, &h);

    *lat = phi_wgs * RAD2DEG;
    *lon = lam_wgs * RAD2DEG;
    return 0;
}

int sh_wgs84_to_eov(double lat, double lon, double *eov_y, double *eov_x)
{
    if (!eov_y || !eov_x) return -1;

    /* Basic range check for Hungary */
    if (lat < 45.5 || lat > 48.7 || lon < 16.0 || lon > 23.0)
        return -1;

    double phi_wgs = lat * DEG2RAD;
    double lam_wgs = lon * DEG2RAD;

    /* Step 1: Geodetic WGS84 → cartesian */
    double X_wgs, Y_wgs, Z_wgs;
    geo_to_xyz(phi_wgs, lam_wgs, 0.0, WGS84_A, WGS84_E2,
               &X_wgs, &Y_wgs, &Z_wgs);

    /* Step 2: Bursa-Wolf WGS84 → HD72 */
    double X_hd72, Y_hd72, Z_hd72;
    bursa_wolf_wgs84_to_hd72(X_wgs, Y_wgs, Z_wgs, &X_hd72, &Y_hd72, &Z_hd72);

    /* Step 3: Cartesian HD72 → geodetic GRS67 */
    double phi_hd72, lam_hd72, h;
    xyz_to_geo(X_hd72, Y_hd72, Z_hd72, GRS67_A, GRS67_E2,
               &phi_hd72, &lam_hd72, &h);

    /* Step 4: Geodetic GRS67 → EOV */
    geo_to_eov(phi_hd72, lam_hd72, eov_y, eov_x);
    return 0;
}
