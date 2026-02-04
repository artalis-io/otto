/**
 * ClayShards Map Provider - JavaScript Bridge
 *
 * Polls WASM for pending requests and makes fetch calls to API servers.
 * Routes responses back to WASM callbacks.
 *
 * Uses resilient-fetch for automatic retry with circuit breaker protection.
 */

import { createResilientFetch, CircuitBreaker } from '../../shared/js/index.js';

export class MapProvider {
    /**
     * Create a MapProvider.
     *
     * @param {Object} wasm WASM module instance
     * @param {Object} options Configuration options
     * @param {number} options.timeoutMs Request timeout (default: 30000)
     * @param {number} options.maxRetries Maximum retry attempts (default: 3)
     * @param {number} options.circuitFailureThreshold Failures before circuit opens (default: 5)
     */
    constructor(wasm, {
        timeoutMs = 30000,
        maxRetries = 3,
        circuitFailureThreshold = 5
    } = {}) {
        this.wasm = wasm;
        this.polling = false;
        this.pollInterval = null;

        // Create resilient fetch clients for each API
        this._routeClient = createResilientFetch({
            circuit: new CircuitBreaker({ failureThreshold: circuitFailureThreshold }),
            timeoutMs,
            maxRetries
        });

        this._geocodeClient = createResilientFetch({
            circuit: new CircuitBreaker({ failureThreshold: circuitFailureThreshold }),
            timeoutMs,
            maxRetries
        });
    }

    /**
     * Get route client circuit breaker state.
     * @returns {string} Circuit state
     */
    getRouteCircuitState() {
        return this._routeClient.circuit.getState();
    }

    /**
     * Get geocode client circuit breaker state.
     * @returns {string} Circuit state
     */
    getGeocodeCircuitState() {
        return this._geocodeClient.circuit.getState();
    }

    /**
     * Start polling for pending requests
     */
    start(intervalMs = 50) {
        if (this.polling) return;
        this.polling = true;
        this.pollInterval = setInterval(() => this._poll(), intervalMs);
    }

    /**
     * Stop polling
     */
    stop() {
        this.polling = false;
        if (this.pollInterval) {
            clearInterval(this.pollInterval);
            this.pollInterval = null;
        }
    }

    /**
     * Poll for pending requests
     */
    _poll() {
        if (!this.wasm) return;

        // Check for pending route request
        if (this.wasm.cs_provider_route_is_pending()) {
            this._fetchRoute();
        }

        // Check for pending search request
        if (this.wasm.cs_provider_search_is_pending()) {
            this._fetchSearch();
        }

        // Check for pending reverse geocode request
        if (this.wasm.cs_provider_reverse_is_pending()) {
            this._fetchReverse();
        }
    }

    /**
     * Fetch route from Velo API
     */
    async _fetchRoute() {
        const wasm = this.wasm;

        // Mark as fetching (prevents duplicate requests)
        wasm.cs_provider_route_mark_fetching();

        const fromLat = wasm.cs_provider_route_from_lat();
        const fromLon = wasm.cs_provider_route_from_lon();
        const toLat = wasm.cs_provider_route_to_lat();
        const toLon = wasm.cs_provider_route_to_lon();
        const server = this._getString(wasm.cs_provider_route_server());

        // Get profile and mode from WASM
        const profileNum = wasm.cs_provider_get_route_profile();
        const modeNum = wasm.cs_provider_get_route_mode();
        const profile = profileNum === 1 ? 'truck' : 'car';
        const mode = modeNum === 1 ? 'shortest' : 'fastest';

        const url = `${server}/api/v1/route?from=${encodeURIComponent(`${fromLat},${fromLon}`)}&to=${encodeURIComponent(`${toLat},${toLon}`)}&profile=${encodeURIComponent(profile)}&mode=${encodeURIComponent(mode)}&geometry=true`;

        const startTime = performance.now();

        try {
            const response = await this._routeClient.fetch(url);
            const data = await response.json();
            const calcTimeMs = performance.now() - startTime;

            if (data.error) {
                this._reportRouteError(data.error);
                return;
            }

            // Extract route data (Velo API nests under 'route')
            const route = data.route || data;
            const geometryStr = route.geometry || '';

            // Decode Google Polyline format
            const coords = this._decodePolyline(geometryStr);
            const lats = new Float64Array(coords.length);
            const lons = new Float64Array(coords.length);

            for (let i = 0; i < coords.length; i++) {
                lats[i] = coords[i][0];
                lons[i] = coords[i][1];
            }

            // Pass to WASM
            const latsPtr = this._allocDoubleArray(lats);
            const lonsPtr = this._allocDoubleArray(lons);

            wasm.cs_provider_on_route_complete(
                latsPtr, lonsPtr, coords.length,
                route.distance || 0,
                route.duration || 0,
                calcTimeMs
            );

            // Free temporary arrays
            wasm.free(latsPtr);
            wasm.free(lonsPtr);

        } catch (err) {
            const message = err.isCircuitOpen
                ? 'Route service unavailable (circuit breaker open)'
                : err.message;
            this._reportRouteError(message);
        }
    }

    /**
     * Fetch search results from Locus API
     */
    async _fetchSearch() {
        const wasm = this.wasm;

        wasm.cs_provider_search_mark_fetching();

        const query = this._getString(wasm.cs_provider_search_query());
        const server = this._getString(wasm.cs_provider_geocode_server());

        let url = `${server}/api/v1/search?q=${encodeURIComponent(query)}`;

        if (wasm.cs_provider_search_has_bias()) {
            const lat = wasm.cs_provider_search_bias_lat();
            const lon = wasm.cs_provider_search_bias_lon();
            url += `&lat=${lat}&lon=${lon}`;
        }

        try {
            const response = await this._geocodeClient.fetch(url);
            const data = await response.json();

            if (data.error) {
                this._reportSearchError(data.error);
                return;
            }

            const results = data.results || [];
            const count = Math.min(results.length, 10); // CS_PROVIDER_MAX_SEARCH_RESULTS

            // Set each result individually
            for (let i = 0; i < count; i++) {
                const r = results[i];
                const namePtr = this._allocString(r.name || '');
                const typePtr = this._allocString(r.type || '');
                wasm.cs_provider_set_search_result(
                    i,
                    r.lat || 0,
                    r.lon || 0,
                    namePtr,
                    typePtr,
                    r.score || 0
                );
                wasm.free(namePtr);
                wasm.free(typePtr);
            }

            wasm.cs_provider_on_search_complete(count);

        } catch (err) {
            const message = err.isCircuitOpen
                ? 'Geocode service unavailable (circuit breaker open)'
                : err.message;
            this._reportSearchError(message);
        }
    }

    /**
     * Fetch reverse geocode from Locus API
     */
    async _fetchReverse() {
        const wasm = this.wasm;

        wasm.cs_provider_reverse_mark_fetching();

        const lat = wasm.cs_provider_reverse_lat();
        const lon = wasm.cs_provider_reverse_lon();
        const server = this._getString(wasm.cs_provider_geocode_server());

        const url = `${server}/api/v1/reverse?lat=${encodeURIComponent(lat)}&lon=${encodeURIComponent(lon)}`;

        try {
            const response = await this._geocodeClient.fetch(url);
            const data = await response.json();

            if (data.error) {
                this._reportReverseError(data.error);
                return;
            }

            // Locus API returns: display_name, place, street
            // Map to our fields: name, street, city, country
            const namePtr = this._allocString(data.display_name || data.name || '');
            const streetPtr = this._allocString(data.street || '');
            const cityPtr = this._allocString(data.place || data.city || '');
            const countryPtr = this._allocString(data.country || '');

            wasm.cs_provider_on_reverse_complete(namePtr, streetPtr, cityPtr, countryPtr);

            wasm.free(namePtr);
            wasm.free(streetPtr);
            wasm.free(cityPtr);
            wasm.free(countryPtr);

        } catch (err) {
            const message = err.isCircuitOpen
                ? 'Geocode service unavailable (circuit breaker open)'
                : err.message;
            this._reportReverseError(message);
        }
    }

    /**
     * Report route error to WASM
     */
    _reportRouteError(message) {
        const ptr = this._allocString(message);
        this.wasm.cs_provider_on_route_error(ptr);
        this.wasm.free(ptr);
    }

    /**
     * Report search error to WASM
     */
    _reportSearchError(message) {
        const ptr = this._allocString(message);
        this.wasm.cs_provider_on_search_error(ptr);
        this.wasm.free(ptr);
    }

    /**
     * Report reverse error to WASM
     */
    _reportReverseError(message) {
        const ptr = this._allocString(message);
        this.wasm.cs_provider_on_reverse_error(ptr);
        this.wasm.free(ptr);
    }

    /**
     * Read a C string from WASM memory
     */
    _getString(ptr) {
        if (!ptr) return '';
        const memory = new Uint8Array(this.wasm.memory.buffer);
        const maxLen = memory.length;
        let end = ptr;
        // Bounds check: don't read past WASM memory
        while (end < maxLen && memory[end] !== 0) end++;
        if (end >= maxLen) return '';  // Unterminated string
        const bytes = memory.slice(ptr, end);
        return new TextDecoder().decode(bytes);
    }

    /**
     * Allocate a string in WASM memory
     * Note: Caller should free if needed, but for short-lived strings
     * passed to callbacks this is handled by the C side
     */
    _allocString(str) {
        const bytes = new TextEncoder().encode(str + '\0');
        const ptr = this.wasm.malloc(bytes.length);
        const memory = new Uint8Array(this.wasm.memory.buffer);
        memory.set(bytes, ptr);
        return ptr;
    }

    /**
     * Allocate a Float64Array in WASM memory
     * Note: View is created AFTER malloc to handle potential memory growth
     */
    _allocDoubleArray(arr) {
        const ptr = this.wasm.malloc(arr.length * 8);
        // Get fresh buffer reference after malloc (may have triggered memory growth)
        const view = new Float64Array(this.wasm.memory.buffer, ptr, arr.length);
        view.set(arr);
        return ptr;
    }

    /**
     * Decode Google Polyline encoded string to array of [lat, lon] pairs
     */
    _decodePolyline(encoded) {
        const coords = [];
        let index = 0;
        let lat = 0;
        let lon = 0;

        while (index < encoded.length) {
            // Decode latitude
            let shift = 0;
            let result = 0;
            let byte;
            do {
                byte = encoded.charCodeAt(index++) - 63;
                result |= (byte & 0x1f) << shift;
                shift += 5;
            } while (byte >= 0x20);
            const dlat = (result & 1) ? ~(result >> 1) : (result >> 1);
            lat += dlat;

            // Decode longitude
            shift = 0;
            result = 0;
            do {
                byte = encoded.charCodeAt(index++) - 63;
                result |= (byte & 0x1f) << shift;
                shift += 5;
            } while (byte >= 0x20);
            const dlon = (result & 1) ? ~(result >> 1) : (result >> 1);
            lon += dlon;

            coords.push([lat / 1e5, lon / 1e5]);
        }

        return coords;
    }
}
