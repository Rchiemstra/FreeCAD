CI image identity and migration
==============================

The agent Docker daemon must run on the same host that publishes the registry
at 127.0.0.1:5001. Test that endpoint on the daemon host. For a remote agent,
use a TLS registry, or configure the LAN endpoint in its Docker daemon's
insecure-registries and update the workflow pull references. In-container
publication uses registry-freecad:5000. The optional local builder is
ci/build-images.sh; it requires Docker and crane (v0.20.3 or newer) on PATH.
The local Docker daemon must reach REGISTRY and permit that HTTP registry.

After dependency edits, MCP dependency changes, or upstream merges, initialize
MCP at the recorded commit, run `sh ci/woodpecker/ci-images-key.sh update`, and
commit both .woodpecker YAML files with the input change. CI checks after the
submodule step and rejects stale keys before any heavy pull. MCP identity hashes
only its Dockerfile.ci and pyproject.toml plus the deps key: runtime reinstalls
workspace source, and the image deletes the baked source. Source-only gitlink
updates therefore keep dependency keys. Revisit these inputs if the Dockerfile
starts keeping source or otherwise deriving installed dependencies from it.

For an external apt/pip/base refresh, edit ci/images-refresh.txt and regenerate
both workflows. Do not add refresh comments to upstream package Dockerfiles.
Cron/manual runs repair missing images and aliases; they do not refresh an
existing input key. Initial adoption changes both keys; upgrade active branches
to get the parent-aware gate and shared publication lock. Older PR configs can
continue pulling :24.04, but the alias is rolling and has no isolation guarantee
when branches have different inputs.

On push/tag/manual, ci.yml waits at most 90 checks, 10 seconds apart, for a
concurrent build-images workflow before doing its own build. build-images.yml
never waits for itself. PRs have no companion producer, so they do not wait.
Both workflows and the local builder build without pushing, then take the same
publisher lock, recheck the keyed manifest immediately before publishing, and
use an image that appeared while building instead of overwriting it, except for
the stale-parent MCP repair described below. No-op
paths copy the chosen registry digest to :24.04, repairing missing/stale aliases.
Network/auth/server errors fail closed; only explicit missing manifests permit
publication. MCP builds FROM the resolved deps digest, labels it with
org.freecad.ci.deps-digest, and the readiness gate compares that label to deps.

Both workflows require trusted bind mounts for
/tmp/freecad-ci-publish:/tmp/freecad-ci-publish. Local publishers must run on
that same daemon host and share /tmp/freecad-ci-publish (created mode 1777 so
CI root and the local user can both lock it), or the same explicitly
configured FREECAD_CI_PUBLISH_LOCK_ROOT. A build can occur outside the lock;
only its short publication/alias phase is serialized. A killed process may
leave /tmp/freecad-ci-publish/publisher: after the bounded timeout, inspect
publishers and remove that directory only when no owner remains. Do not remove
a live lock. Builds on another host or older publisher scripts do not honor
this lock; the registry API has no atomic create-if-absent operation. Such
publishers must be stopped/migrated before enabling this protocol. Merely
rechecking a tag without the shared lock would still permit a concurrent push.

A present MCP key can be replaced only when its recorded deps parent digest
differs from the current deps digest. Under the publication lock, the builder
rechecks both the current deps digest and the existing MCP parent, then logs
the old MCP digest, old parent and new parent before publishing its replacement.
The readiness gate records the resulting MCP digest and parent in identity.txt.
Matching-parent MCP keys and existing deps keys are never overwritten. An MCP
key without a recorded parent is also protected: change ci/images-refresh.txt
and regenerate both keys to migrate it. Missing deps always marks MCP for
repair, even when MCP still exists.

The final identity.txt is a verified snapshot, not a retention lease. Mid-pipeline
eviction after verification can still break a later pull, and consumers are
not yet bound to those recorded digests. Retention/pinning through pipeline
completion remains open. MCP shards and other failure-continued steps retain
their base step names, depends_on targets and when filters, so a sibling failure
(including e2e) does not suppress the remaining failure-enabled shards; one run
can report all failures. If the image gate itself failed, these steps can fail
at image pull on an already red pipeline. Their failure filters do not guarantee
that the image gate succeeded or that a keyed image is still available.
