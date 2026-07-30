# Complete async LOCKED_ERROR handoff flow

This is an as-implemented snapshot of the combined staged and unstaged
`tools/mcp/freecad-mcp` working tree, reviewed on 2026-07-31.

Final review:

- The focused lease/handoff suite passes: 194 tests across
  `test_rpc_dirty_adoption.py`, `test_rpc_request_idempotency.py`,
  `test_lease_manager.py`, `test_mcp_rpc_v2_lifecycle.py`, and
  `test_document_lease_v2_service.py`.
- The changed Python modules pass `compileall`, and `git diff --check HEAD`
  reports no whitespace errors.
- The default handoff authorization hook always returns `true`; there is no
  FreeCAD confirmation popup. The `denied` continuation state remains as a
  defensive/testable branch.
- Handoff eligibility is based on an eligible local dirty `LOCKED_ERROR`
  record and its recovery evidence; the service does not enforce that the
  replacement `agent_id` or MCP runtime differs from the recorded owner.
- `cancel_request` is currently registered as a model-facing MCP tool. The
  lower-level client/RPC docstrings that still call it unexposed are stale
  comments, not runtime behavior.
- Exact continuation states are `pending_authorization`, `authorizing`,
  `hashing`, `claiming`, `claim_committed`, `claiming_uncertain`, `claimable`,
  `cancelled`, `failed`, `denied`, and `claimed`.
- The credential vault is process-local and defaults to 256 entries with a
  600-second TTL. A claimable continuation whose vault entry expires becomes a
  recovery-required failure when `claim_acquisition_result` observes the
  missing credential.
- `invoke_v2` finalizes the original acquisition handler before MCP-local
  credential storage and the separate custody acknowledgement. The replay
  cache never stores a raw acquisition token.

```mermaid
sequenceDiagram
    autonumber

    actor Agent
    participant MCP as MCP Server
    participant LM as MCP Lease Manager
    participant RPC as FreeCAD RPC
    participant IR as Inflight Request Registry
    participant HC as Handoff Continuation
    participant GUI as FreeCAD GUI Thread
    participant LS as Lease Service
    participant Vault as Credential Vault

    Note over Agent,Vault: Entry and synchronous adoption/acquisition

    Agent->>MCP: adopt_dirty_document(selector)
    Note over MCP,LM: The same successful custody/redaction path also applies<br/>to acquire_document_lock success responses
    MCP->>RPC: invoke_v2(adopt_dirty_document, acquisition_request_id,<br/>live_request_ids, authenticated)
    RPC->>IR: Register live invoke_v2 request
    RPC->>GUI: Resolve selector and inspect document

    alt Selector is invalid, missing, or ambiguous
        GUI-->>RPC: Resolution failure
        RPC->>IR: Finalize request as failed
        RPC-->>MCP: Terminal error, no credential
        MCP-->>Agent: Failure, no credential

    else Document is not dirty or the adoption precondition is false
        GUI-->>RPC: Nothing eligible to adopt
        RPC->>IR: Finalize request as failed
        RPC-->>MCP: Terminal precondition failure, no credential
        MCP-->>Agent: Tool failure, no lease acquired

    else Document is locked in a non-handoff-eligible state
        GUI-->>RPC: Lock conflict or ownership denial
        RPC->>IR: Finalize request as denied/conflict
        RPC-->>MCP: Terminal denial, no credential
        MCP-->>Agent: Lock not acquired

    else Unlocked eligible document
        Note over GUI,LS: adopt uses begin_dirty_adoption for a dirty document,<br/>acquire uses begin_acquisition for a clean document
        GUI->>LS: Begin ACQUIRING reservation(document identity,<br/>acquisition_request_id, live_request_ids)

        alt Reservation or fencing check rejects acquisition
            LS-->>GUI: Conflict or denial
            GUI-->>RPC: Acquisition not started
            RPC->>IR: Finalize request as failed/denied
            RPC-->>MCP: Terminal result, no credential
            MCP-->>Agent: Adoption/acquisition failed

        else ACQUIRING reservation succeeds
            LS-->>GUI: ACQUIRING reservation
            GUI-->>RPC: Reservation established
            RPC->>RPC: Hash saved-file baseline off GUI thread

            alt Baseline capture fails
                RPC->>GUI: Roll back reservation
                GUI->>LS: abort_acquisition by exact CAS

                alt Exact rollback succeeds
                    LS-->>GUI: Reservation released
                    GUI-->>RPC: Terminal failure, no credential
                else Exact rollback fails
                    LS-->>GUI: Rollback failure
                    Note over GUI,LS: Keep the fence/recovery artifact,<br/>return the stricter rollback error
                    GUI-->>RPC: Terminal recovery failure, no credential
                end
                RPC->>IR: Finalize request as failed
                RPC-->>MCP: Failure, no credential
                MCP-->>Agent: Adoption/acquisition failed

            else Baseline capture succeeds
                RPC->>GUI: Final identity/dirty-state revalidation<br/>and recovery snapshot
                GUI->>LS: Record snapshot and promote ACQUIRING to LOCKED_IDLE

                alt Snapshot, revalidation, or promotion fails
                    GUI->>GUI: Failure detected
                    GUI->>LS: abort_acquisition by exact CAS

                    alt Exact rollback succeeds
                        LS-->>GUI: Reservation released
                        GUI-->>RPC: Terminal failure, no credential
                    else Exact rollback fails
                        LS-->>GUI: Rollback failure
                        Note over GUI,LS: Keep the sidecar and recovery artifact
                        GUI-->>RPC: Terminal recovery failure, no credential
                    end
                    RPC->>IR: Finalize request as failed
                    RPC-->>MCP: Failure, no credential
                    MCP-->>Agent: Adoption/acquisition failed

                else Promotion succeeds
                    LS-->>GUI: Lease metadata and private credential
                    GUI-->>RPC: Successful acquisition result
                    RPC->>Vault: Escrow credential until MCP custody

                    alt Escrow store fails after ownership change
                        Vault-->>RPC: Store failure
                        RPC->>IR: Finalize handler as failed
                        RPC-->>MCP: Failure, token never public
                        MCP-->>Agent: Credential unavailable, recovery required

                    else Escrow succeeds
                        Vault-->>RPC: Credential escrowed
                        RPC->>IR: Finalize acquisition handler as completed
                        Note over RPC,IR: Replay entry is token-free,<br/>the private claim is repeatable until acknowledgement
                        RPC-->>MCP: Private lease result
                        MCP->>LM: Store credential locally

                        alt Local credential storage fails
                            LM-->>MCP: Storage failure
                            Note over MCP,RPC: MCP sends no custody acknowledgement
                            Note over RPC,Vault: Escrow remains for retry or TTL expiry
                            MCP-->>Agent: credential_stored=false, token absent

                        else Local credential storage succeeds
                            LM-->>MCP: Credential stored
                            MCP->>RPC: Acknowledge credential custody

                            alt Custody acknowledgement succeeds
                                RPC->>Vault: Delete acknowledged credential
                                Vault-->>RPC: Deleted
                                RPC-->>MCP: acknowledged=true
                                MCP-->>Agent: success=true, credential_stored=true,<br/>lease metadata retained, token redacted/absent

                            else Custody acknowledgement fails
                                RPC-->>MCP: Retryable acknowledgement failure
                                Note over LM,Vault: MCP holds the credential,<br/>escrow copy remains until retry or TTL
                                MCP-->>Agent: Custody stored, cleanup pending,<br/>token redacted/absent
                            end
                        end
                    end
                end
            end
        end

    else Eligible local dirty LOCKED_ERROR lease requires handoff
        GUI-->>RPC: LOCKED_ERROR handoff required
        RPC->>HC: Start handoff continuation(request_id,<br/>document identity, old ownership)
        RPC->>IR: Finalize detect handler with process_pinned=true
        IR-->>RPC: Terminal failed-status tombstone retained,<br/>control cancellation sees a completed tombstone
        RPC-->>MCP: success=false, error_code=LOCKED_ERROR_HANDOFF_PENDING,<br/>request_id, handoff_pending=true, confirmation_pending=false
        MCP-->>Agent: Non-error CONDITION_FALSE tool result,<br/>success=true, pending=true, credential_stored=false,<br/>no token, poll by request_id

        Note over Agent,GUI: No FreeCAD confirmation popup

        HC->>GUI: Resolve selector, verify dirty state,<br/>and call automatic authorization hook
        Note over HC,GUI: The current default hook always returns true

        alt Defensive authorization hook returns false
            GUI-->>HC: Denied
            HC->>HC: Mark terminal denied, no credential

        else Initial identity or dirty-state validation fails
            GUI-->>HC: Validation failure
            HC->>HC: Mark terminal failed, no credential

        else Initial validation succeeds
            GUI-->>HC: Identity and dirty state valid
            HC->>HC: Capture file baseline off GUI thread

            alt Baseline capture fails
                HC->>HC: Mark terminal failed, no credential

            else Baseline capture succeeds
                HC->>GUI: Final live-document revalidation,<br/>cancel gate, ownership CAS, and immediate escrow

                alt Claim GUI phase times out with uncertain completion
                    GUI-->>HC: completion_uncertain
                    HC->>HC: If escrow is not already claimable,<br/>mark claiming_uncertain (not cancellable)
                    Note over HC,Vault: A late GUI completion may still fail,<br/>or may CAS and escrow the credential

                else Document changed, disappeared, or is no longer eligible
                    GUI-->>HC: Final validation failure
                    HC->>HC: Mark terminal failed, no credential

                else Final revalidation succeeds
                    GUI-->>HC: Document still valid
                    GUI->>HC: begin_claim atomic cancellation gate

                    alt Cancellation won before begin_claim
                        HC->>HC: Mark terminal cancelled, no credential

                    else Claim gate won
                        HC->>HC: Mark claim_committed<br/>(irreversible cancellation boundary)
                        Note over HC,LS: A hang here is not cancellable and does not imply<br/>that an escrowed credential already exists
                        GUI->>LS: CAS rotate LOCKED_ERROR ownership

                        alt Ownership CAS fails
                            LS-->>GUI: CAS failure
                            GUI-->>HC: Claim failure
                            HC->>HC: Mark terminal failed, no credential

                        else Ownership CAS succeeds
                            LS-->>GUI: New lease metadata and private credential
                            GUI->>Vault: Escrow credential immediately

                            alt Escrow store fails after CAS
                                Vault-->>GUI: Store failure
                                GUI-->>HC: Recovery-required failure
                                HC->>HC: Mark recovery-required terminal failure,<br/>ownership already rotated, no claimable credential

                            else Escrow succeeds
                                Vault-->>GUI: Credential escrowed
                                GUI-->>HC: Claim phase completed
                                HC->>HC: Mark claimable
                            end
                        end
                    end
                end
            end
        end
    end

    opt Agent polls after receiving the pending handoff result
        Note over Agent,HC: Status polling after a pending handoff

        loop Until the claim is custodied or a terminal/recovery outcome is known
            Agent->>MCP: get_request_status(request_id)
            MCP->>RPC: Query continuation status
            RPC->>HC: Read authoritative continuation state

            alt pending_authorization, authorizing, hashing, or claiming
                HC-->>RPC: Pre-claim continuation, no credential
                RPC-->>MCP: success=true, state=running,<br/>result_claimable=false
                MCP-->>Agent: Continue polling or cancel

            else claim_committed before CAS/escrow completes
                HC-->>RPC: claim_committed,<br/>credential not yet guaranteed
                RPC-->>MCP: success=true, state=claim_committed,<br/>result_claimable=false
                MCP-->>Agent: Continue polling, do not assume escrow exists

            else claiming_uncertain after a GUI timeout
                HC-->>RPC: claiming_uncertain,<br/>late CAS/escrow outcome unknown
                RPC-->>MCP: success=true, state=running,<br/>completion_uncertain=true,<br/>result_claimable=false
                MCP-->>Agent: Continue polling, cancellation is not allowed

            else claimable with a live vault entry
                HC-->>RPC: continuation state=claimable
                Vault-->>RPC: claimable=true
                RPC-->>MCP: success=true, state=completed,<br/>result_claimable=true
                MCP-->>Agent: Call claim_acquisition_result(request_id)

            else claimable continuation but vault entry expired or is missing
                HC-->>RPC: continuation state=claimable
                Vault-->>RPC: claimable=false
                RPC-->>MCP: success=true, state=completed,<br/>result_claimable=false
                MCP-->>Agent: Call claim once to surface the<br/>credential-unavailable recovery failure

            else terminal cancelled
                HC-->>RPC: cancelled, no credential
                RPC-->>MCP: success=true, state=cancelled
                MCP-->>Agent: Handoff cancelled

            else terminal denied
                HC-->>RPC: denied with details, no credential
                RPC-->>MCP: success=true, state=failed,<br/>handoff_continuation.state=denied
                MCP-->>Agent: Handoff denied, no claim advice

            else terminal failed
                HC-->>RPC: failed with details, no credential
                RPC-->>MCP: success=true, state=failed,<br/>handoff_continuation.state=failed
                MCP-->>Agent: Handoff failed, no claim advice

            else claimed
                HC-->>RPC: continuation state=claimed,<br/>credential already taken into custody
                RPC-->>MCP: success=true, state=completed,<br/>result_claimable=false
                MCP-->>Agent: Lease already stored, no token returned
            end
        end
    end

    opt Agent calls cancel_request at any point after the pending result
        Note over Agent,HC: Cancellation is resolved from continuation state,<br/>not from the inflight tombstone alone

        Agent->>MCP: cancel_request(request_id)
        MCP->>RPC: Cancel invoke_v2/handoff request
        RPC->>IR: request_cancel(request_id)
        IR-->>RPC: requested, completed tombstone, or unknown
        RPC->>HC: Cancel continuation or inspect terminal state

        alt Continuation exists and is cancellable before begin_claim
            HC-->>RPC: cancelled
            Note over RPC,IR: Continuation result overrides tombstone status
            RPC-->>MCP: success=true, handoff_cancelled=true
            MCP-->>Agent: Cancelled, no credential

        else Continuation is claim_committed, claiming_uncertain, or claimable
            HC-->>RPC: not_cancellable with current state
            Note over RPC,IR: Return REQUEST_NOT_CANCELLABLE for every IR status,<br/>including a completed tombstone
            RPC-->>MCP: success=false, error_code=REQUEST_NOT_CANCELLABLE,<br/>neutral irreversible/terminal wording

            alt Current state is claim_committed
                MCP-->>Agent: Continue polling, escrow may not exist yet
            else Current state is claiming_uncertain
                MCP-->>Agent: Continue polling, late CAS/escrow is unresolved
            else Current state is claimable
                MCP-->>Agent: Claim the available acquisition result
            end

        else Continuation is terminal failed
            HC-->>RPC: terminal_failed(details)
            RPC-->>MCP: Return actual failure, not not_cancellable
            MCP-->>Agent: Failure details, no credential to claim

        else Continuation is terminal denied
            HC-->>RPC: terminal_denied(details)
            RPC-->>MCP: Return actual denial, not not_cancellable
            MCP-->>Agent: Denial details, no credential to claim

        else Continuation is already cancelled
            HC-->>RPC: terminal_cancelled
            RPC-->>MCP: Idempotent cancelled result
            MCP-->>Agent: Already cancelled

        else Continuation is already claimed/completed
            HC-->>RPC: terminal_completed
            RPC-->>MCP: Request already completed, cancellation impossible
            MCP-->>Agent: Lease already in custody, no token returned

        else No handoff continuation exists
            HC-->>RPC: not_found
            RPC-->>MCP: Use ordinary inflight cancellation result
            MCP-->>Agent: requested, completed, or unknown as applicable
        end
    end

    opt Agent calls claim_acquisition_result
        Note over Agent,Vault: Claim and one-time credential custody

        Agent->>MCP: claim_acquisition_result(request_id)
        MCP->>RPC: Claim private credential
        RPC->>HC: Inspect authoritative continuation state

        alt Continuation is pending_authorization, authorizing, hashing,<br/>claiming, claim_committed, or claiming_uncertain
            HC-->>RPC: Not claimable yet
            RPC-->>MCP: success=false, pending=true,<br/>error_code=ACQUISITION_CLAIM_PENDING,<br/>no credential
            MCP-->>Agent: Tool failure advises continued polling

        else Continuation is cancelled
            HC-->>RPC: cancelled
            RPC-->>MCP: Terminal cancelled, no credential
            MCP-->>Agent: Nothing to claim

        else Continuation is failed or denied
            HC-->>RPC: Actual terminal state and details
            RPC-->>MCP: Terminal failure/denial, no credential
            MCP-->>Agent: Nothing to claim

        else Continuation is already claimed
            HC-->>RPC: Already claimed
            RPC-->>MCP: success=true, already_claimed=true,<br/>no private token
            MCP-->>Agent: Lease already stored, token absent

        else Continuation is claimable
            HC-->>RPC: Claimable
            RPC->>Vault: Read escrowed credential

            alt Credential expired or is missing
                Vault-->>RPC: Missing/expired
                RPC->>HC: Mark credential unavailable/expired
                RPC-->>MCP: Terminal credential-unavailable failure
                MCP-->>Agent: No token, ownership may require recovery

            else Credential is available
                Vault-->>RPC: Lease metadata and private token
                Note over RPC,Vault: The claim is a repeatable private peek until acknowledgement
                RPC-->>MCP: Private credential result
                MCP->>LM: Store credential locally

                alt Local credential storage fails
                    LM-->>MCP: Storage failure
                    Note over MCP,RPC: MCP sends no custody acknowledgement
                    Note over RPC,Vault: Keep escrow for retry until TTL,<br/>never expose token publicly
                    MCP-->>Agent: credential_stored=false, token absent

                else Local credential storage succeeds
                    LM-->>MCP: Credential stored
                    MCP->>RPC: Acknowledge custody

                    alt Acknowledgement succeeds
                        RPC->>Vault: Delete acknowledged credential
                        Vault-->>RPC: Deleted
                        RPC->>HC: Mark claimed
                        RPC-->>MCP: Custody complete
                        MCP-->>Agent: Handoff completed, credential_stored=true,<br/>lease metadata retained, token redacted/absent

                    else Acknowledgement fails
                        RPC-->>MCP: Retryable acknowledgement failure
                        Note over LM,Vault: MCP holds token,<br/>escrow copy remains until retry or TTL
                        MCP-->>Agent: Custody stored, cleanup pending,<br/>token redacted/absent
                    end
                end
            end
        end
    end

    opt Independent create_document lifecycle
        Note over Agent,LS: create_document must use the same acquisition fencing identity

        Agent->>MCP: create_document(name)
        MCP->>RPC: invoke_v2(create_document, acquisition_request_id,<br/>live_request_ids, authenticated)
        RPC->>IR: Register live create request
        RPC->>GUI: Reject an already-open name, otherwise create document
        GUI->>LS: begin_acquisition(document_id,<br/>acquisition_request_id, live_request_ids)

        alt Stale same-MCP ACQUIRING owner is not in live_request_ids
            LS->>LS: Fence interrupted request immediately
            LS-->>GUI: New ACQUIRING reservation
            Note over GUI,Vault: Create recovery snapshot, promote, then use the common<br/>escrow, local custody, acknowledgement, and redaction path

        else Same-MCP acquisition request is still live
            LS-->>GUI: Do not steal, return conflict/pending owner
            GUI-->>RPC: Creation/acquisition not completed
            RPC-->>MCP: Non-success result, no credential
            MCP-->>Agent: Existing request remains authoritative

        else Foreign owner or non-fenceable lock state exists
            LS-->>GUI: Lock conflict/denial
            GUI->>LS: Attempt exact reservation rollback
            GUI-->>RPC: Creation/acquisition failed
            RPC-->>MCP: Terminal result, no credential
            MCP-->>Agent: New document is normally closed if rollback succeeds,<br/>a failed rollback deliberately retains the open fenced document for recovery

        else No conflicting acquisition exists
            LS-->>GUI: ACQUIRING reservation
            GUI->>GUI: Create recovery snapshot
            GUI->>LS: complete_acquisition to LOCKED_IDLE
            Note over GUI,Vault: Continue through the common escrow, local custody,<br/>acknowledgement, and public token redaction path
        end
    end
```
