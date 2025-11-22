CREATE TABLE locations2025 (
    id SERIAL PRIMARY KEY,
    latitude DOUBLE PRECISION NOT NULL,
    longitude DOUBLE PRECISION NOT NULL,
    short_address VARCHAR(255),
    long_address VARCHAR(1024),
    precision INTEGER CHECK (precision BETWEEN 0 AND 15),
    source VARCHAR(16) CHECK (source IN ('history', 'api', 'postgis', 'nom', 'python'))
);

CREATE TABLE locations2025 (
    id SERIAL PRIMARY KEY,
    latitude DOUBLE PRECISION NOT NULL,
    longitude DOUBLE PRECISION NOT NULL,
    short_address VARCHAR(255),
    long_address VARCHAR(1024),
    precision INTEGER CHECK (precision BETWEEN 0 AND 15),
    source VARCHAR(16) CHECK (source IN ('history', 'api', 'postgis', 'nom', 'python'))
);

