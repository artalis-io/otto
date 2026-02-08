# FuelWise UI

React frontend for the FuelWise truck refueling optimization platform.

## Features

- Interactive map with Leaflet
- CSV upload for stations and waypoints
- Route visualization via OSRM
- Vehicle configuration with variable consumption
- Optimization results display

## Quick Start

```bash
npm install
npm run dev
```

Requires the API server running on port 8080:
```bash
cd ../api && make run
```

## Build

```bash
npm run build
npm run preview
```

## License

AGPLv3 + Commercial License with Trucking Exception - see [LICENSE](../../LICENSE).
