VM Wall — legacy single-host viewer

This directory contains the previous Node.js implementation. The current
multi-host viewer and installer are documented in the repository README.

Use npm ci, npm test and npm run build from this directory. Set VIEWER_DATA
to the private directory for captures and configuration. Run npm start to
start the service on loopback. This legacy viewer has no password login.
