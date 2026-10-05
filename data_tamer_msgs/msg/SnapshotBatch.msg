# Several snapshots aggregated into one message, to reduce the publish rate.
#
# `schemas` holds the schema of every snapshot in `snapshots` when the
# publisher embeds them, which makes the message self-contained: it can be
# decoded without the `schemas` topic. It is empty otherwise.
Schema[] schemas
Snapshot[] snapshots
