CREATE TABLE locations2025 (
    id SERIAL PRIMARY KEY,
    longitude DOUBLE PRECISION NOT NULL,
    latitude DOUBLE PRECISION NOT NULL,
    short_address VARCHAR(255),
    long_address VARCHAR(1024),
    precision INTEGER CHECK (precision BETWEEN 0 AND 15),
    source VARCHAR(16) CHECK (source IN ('history', 'api', 'postgis', 'nom', 'python'))
);

CREATE INDEX idx_locations2025_longitude
    ON locations2025 (longitude);

CREATE INDEX idx_locations2025_latitude
    ON locations2025 (latitude);

