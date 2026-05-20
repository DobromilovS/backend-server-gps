CREATE TABLE IF NOT EXISTS location_data (
    id SERIAL PRIMARY KEY,
    latitude DOUBLE PRECISION,
    longitude DOUBLE PRECISION,
    altitude DOUBLE PRECISION,
    timestamp BIGINT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);


CREATE TABLE IF NOT EXISTS cell_data (
    id SERIAL PRIMARY KEY,
    location_id INTEGER REFERENCES location_data(id),
    cell_type VARCHAR(10),
    ci INTEGER,
    earfcn INTEGER,
    pci INTEGER,
    tac INTEGER,
    rsrp INTEGER,
    rsrq INTEGER,
    rssi INTEGER,
    ta INTEGER,
    lac INTEGER,
    nci BIGINT,
    ss_rsrp INTEGER,
    ss_rsrq INTEGER,
    ss_sinr INTEGER,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_location_timestamp ON location_data(timestamp);
CREATE INDEX IF NOT EXISTS idx_cell_data_location_id ON cell_data(location_id);
CREATE INDEX IF NOT EXISTS idx_cell_data_type ON cell_data(cell_type);
CREATE INDEX IF NOT EXISTS idx_cell_data_earfcn ON cell_data(earfcn);
