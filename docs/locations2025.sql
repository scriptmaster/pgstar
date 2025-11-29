CREATE TABLE locations2025 (
    id SERIAL PRIMARY KEY,
    long      NUMERIC(9,4) NOT NULL,
    lat       NUMERIC(9,4) NOT NULL,
    long_raw  DOUBLE PRECISION,
    lat_raw   DOUBLE PRECISION,
    short_address VARCHAR(255),
    long_address  VARCHAR(1024),
    precision INTEGER DEFAULT 4 CHECK (precision BETWEEN 0 AND 15),
    source VARCHAR(16) CHECK (source IN ('history', 'api', 'postgis', 'nom', 'python'))
);

CREATE INDEX idx_locations2025_long
    ON locations2025 (long);

CREATE INDEX idx_locations2025_lat
    ON locations2025 (lat);

CREATE UNIQUE INDEX IF NOT EXISTS ux_locations2025_unique_loc_addr
    ON locations2025 (long, lat, short_address, long_address);

