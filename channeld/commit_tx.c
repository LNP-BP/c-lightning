#include <bitcoin/script.h>
#include <bitcoin/tx.h>
#include <ccan/endian/endian.h>
#include <channeld/commit_tx.h>
#include <common/htlc_trim.h>
#include <common/htlc_tx.h>
#include <common/keyset.h>
#include <common/permute_tx.h>
#include <common/utils.h>

#ifndef SUPERVERBOSE
#define SUPERVERBOSE(...)
#endif

static bool trim(const struct htlc *htlc,
		 u32 feerate_per_kw,
		 struct amount_sat dust_limit,
		 enum side side)
{
	return htlc_is_trimmed(htlc_owner(htlc), htlc->amount,
			       feerate_per_kw, dust_limit, side);
}

size_t commit_tx_num_untrimmed(const struct htlc **htlcs,
			       u32 feerate_per_kw,
			       struct amount_sat dust_limit,
			       enum side side)
{
	size_t i, n;

	for (i = n = 0; i < tal_count(htlcs); i++)
		n += !trim(htlcs[i], feerate_per_kw, dust_limit, side);

	return n;
}

static void add_offered_htlc_out(struct bitcoin_tx *tx, size_t n,
				 const struct htlc *htlc,
				 const struct keyset *keyset,
				 struct witscript *o_wscript)
{
	struct ripemd160 ripemd;
	u8 *wscript, *p2wsh;
	struct amount_sat amount = amount_msat_to_sat_round_down(htlc->amount);

	ripemd160(&ripemd, htlc->rhash.u.u8, sizeof(htlc->rhash.u.u8));
	wscript = htlc_offered_wscript(tx, &ripemd, keyset);
	p2wsh = scriptpubkey_p2wsh(tx, wscript);
	bitcoin_tx_add_output(tx, p2wsh, amount);
	SUPERVERBOSE("# HTLC %" PRIu64 " offered %s wscript %s\n", htlc->id,
		     type_to_string(tmpctx, struct amount_sat, &amount),
		     tal_hex(wscript, wscript));
	o_wscript->ptr = tal_dup_arr(o_wscript, u8, wscript,
				     tal_count(wscript), 0);
	tal_free(wscript);
}

static void add_received_htlc_out(struct bitcoin_tx *tx, size_t n,
				  const struct htlc *htlc,
				  const struct keyset *keyset,
				  struct witscript *o_wscript)
{
	struct ripemd160 ripemd;
	u8 *wscript, *p2wsh;
	struct amount_sat amount;

	ripemd160(&ripemd, htlc->rhash.u.u8, sizeof(htlc->rhash.u.u8));
	wscript = htlc_received_wscript(tx, &ripemd, &htlc->expiry, keyset);
	p2wsh = scriptpubkey_p2wsh(tx, wscript);
	amount = amount_msat_to_sat_round_down(htlc->amount);

	bitcoin_tx_add_output(tx, p2wsh, amount);

	SUPERVERBOSE("# HTLC %"PRIu64" received %s wscript %s\n",
		     htlc->id,
		     type_to_string(tmpctx, struct amount_sat,
				    &amount),
		     tal_hex(wscript, wscript));
	o_wscript->ptr = tal_dup_arr(o_wscript, u8,
				     wscript, tal_count(wscript), 0);
	tal_free(wscript);
}

struct bitcoin_tx *commit_tx(const tal_t *ctx,
			     const struct bitcoin_txid *funding_txid,
			     unsigned int funding_txout,
			     struct amount_sat funding,
			     enum side opener,
			     u16 to_self_delay,
			     const struct keyset *keyset,
			     u32 feerate_per_kw,
			     struct amount_sat dust_limit,
			     struct amount_msat self_pay,
			     struct amount_msat other_pay,
			     const struct htlc **htlcs,
			     const struct htlc ***htlcmap,
			     struct wally_tx_output *direct_outputs[NUM_SIDES],
			     u64 obscured_commitment_number,
			     enum side side)
{
	struct amount_sat base_fee;
	struct amount_msat total_pay;
	struct bitcoin_tx *tx;
	size_t i, n, untrimmed;
	u32 *cltvs;
	struct htlc *dummy_to_local = (struct htlc *)0x01,
		*dummy_to_remote = (struct htlc *)0x02;
	if (!amount_msat_add(&total_pay, self_pay, other_pay))
		abort();
	assert(!amount_msat_greater_sat(total_pay, funding));

	/* RGB
	 *
	 * In this function we have to add an LNPBP1-4 commitment to
	 * some client-validated state data (RGB data) into
	 * the LN commitment transaction. For this reason we need to:
	 * - tweak a single public key in one of the transaction
	 *   outputs according to LNPBP-1
	 * - make sure that `(fee + <RGB-specific data>) mod num_outputs`
	 *   points to the output containing the tweaked key
	 *
	 * Issues that we have to keep in mind:
	 * 1. Number of commitment transaction outputs may vary and be
	 *    up to a thousands (because of multiple HTLCs), adjusting
	 *    the fee may take up to thousands of satoshis
	 * 2. `to_local` and `to_remote` outputs may be absent
	 *    from the transaction (when all funds are allocated to
	 *    HTLCs)
	 * 3. Increasing the fee requires to take the funds from
	 *    somewhere, which may exceed amount of funds available
	 *    in both `to_local` and `to_remote` outputs
	 *
	 * Thee are two ways of doing that:
	 * 1. Update the fee:
	 *    - decide where to take the funds in a deterministic way
	 *      and fail if there is no enough funds available
	 *    - put the commitment into `to_local` and, if absent,
	 *      `to_remote` output
	 *    - tweak only a copy of the public key from
	 *      `keyset` and do not modify it
	 *    - fail if no `to_local` and `to_remote` outputs are
	 *      present
	 * 2. Put the commitment into any of the outputs matching
	 *    the present fee:
	 *    - tweak the key from the specific output
	 *    - re-generate scriptPubkey for the output
	 *    - if it is an HTLC output, re-generate HTLC success
	 *      or failure transaction
	 *
	 * We choose the second option since it has less tradeoffs
	 * and is more deterministic
	 *
	 */

	/* BOLT #3:
	 *
	 * 1. Calculate which committed HTLCs need to be trimmed (see
	 * [Trimmed Outputs](#trimmed-outputs)).
	 */
	untrimmed = commit_tx_num_untrimmed(htlcs,
					    feerate_per_kw,
					    dust_limit, side);

	/* BOLT #3:
	 *
	 * 2. Calculate the base [commitment transaction
	 * fee](#fee-calculation).
	 */
	base_fee = commit_tx_base_fee(feerate_per_kw, untrimmed);

	SUPERVERBOSE("# base commitment transaction fee = %s\n",
		     type_to_string(tmpctx, struct amount_sat, &base_fee));

	/* BOLT #3:
	 *
	 * 3. Subtract this base fee from the funder (either `to_local` or
	 * `to_remote`), with a floor of 0 (see [Fee Payment](#fee-payment)).
	 */
	try_subtract_fee(opener, side, base_fee, &self_pay, &other_pay);

#ifdef PRINT_ACTUAL_FEE
	{
		struct amount_sat out = AMOUNT_SAT(0);
		bool ok = true;
		for (i = 0; i < tal_count(htlcs); i++) {
			if (!trim(htlcs[i], feerate_per_kw, dust_limit, side))
				ok &= amount_sat_add(&out, out, amount_msat_to_sat_round_down(htlcs[i]->amount));
		}
		if (amount_msat_greater_sat(self_pay, dust_limit))
			ok &= amount_sat_add(&out, out, amount_msat_to_sat_round_down(self_pay));
		if (amount_msat_greater_sat(other_pay, dust_limit))
			ok &= amount_sat_add(&out, out, amount_msat_to_sat_round_down(other_pay));
		assert(ok);
		SUPERVERBOSE("# actual commitment transaction fee = %"PRIu64"\n",
			     funding.satoshis - out.satoshis);  /* Raw: test output */
	}
#endif

	/* Worst-case sizing: both to-local and to-remote outputs. */
	tx = bitcoin_tx(ctx, chainparams, 1, untrimmed + 2, 0);

	/* We keep track of which outputs have which HTLCs */
	*htlcmap = tal_arr(tx, const struct htlc *, tx->wtx->outputs_allocation_len);

	/* We keep cltvs for tie-breaking HTLC outputs; we use the same order
	 * for sending the htlc txs, so it may matter. */
	cltvs = tal_arr(tmpctx, u32, tx->wtx->outputs_allocation_len);

	/* This could be done in a single loop, but we follow the BOLT
	 * literally to make comments in test vectors clearer. */

	n = 0;
	/* BOLT #3:
	 *
	 * 3. For every offered HTLC, if it is not trimmed, add an
	 *    [offered HTLC output](#offered-htlc-outputs).
	 */
	for (i = 0; i < tal_count(htlcs); i++) {
		if (htlc_owner(htlcs[i]) != side)
			continue;
		if (trim(htlcs[i], feerate_per_kw, dust_limit, side))
			continue;
		tx->output_witscripts[n] =
			tal(tx->output_witscripts, struct witscript);
		add_offered_htlc_out(tx, n, htlcs[i],
				     keyset, tx->output_witscripts[n]);
		(*htlcmap)[n] = htlcs[i];
		cltvs[n] = abs_locktime_to_blocks(&htlcs[i]->expiry);
		n++;
	}

	/* BOLT #3:
	 *
	 * 4. For every received HTLC, if it is not trimmed, add an
	 *    [received HTLC output](#received-htlc-outputs).
	 */
	for (i = 0; i < tal_count(htlcs); i++) {
		if (htlc_owner(htlcs[i]) == side)
			continue;
		if (trim(htlcs[i], feerate_per_kw, dust_limit, side))
			continue;
		tx->output_witscripts[n] =
			tal(tx->output_witscripts, struct witscript);
		add_received_htlc_out(tx, n, htlcs[i], keyset,
				      tx->output_witscripts[n]);
		(*htlcmap)[n] = htlcs[i];
		cltvs[n] = abs_locktime_to_blocks(&htlcs[i]->expiry);
		n++;
	}

	/* BOLT #3:
	 *
	 * 5. If the `to_local` amount is greater or equal to
	 *    `dust_limit_satoshis`, add a [`to_local`
	 *    output](#to_local-output).
	 */
	if (amount_msat_greater_eq_sat(self_pay, dust_limit)) {
		u8 *wscript = to_self_wscript(tmpctx, to_self_delay,keyset);
		u8 *p2wsh = scriptpubkey_p2wsh(tx, wscript);
		struct amount_sat amount = amount_msat_to_sat_round_down(self_pay);

		bitcoin_tx_add_output(tx, p2wsh, amount);
		/* Add a dummy entry to the htlcmap so we can recognize it later */
		(*htlcmap)[n] = direct_outputs ? dummy_to_local : NULL;
		/* We don't assign cltvs[n]: if we use it, order doesn't matter.
		 * However, valgrind will warn us something wierd is happening */
		SUPERVERBOSE("# to-local amount %s wscript %s\n",
			     type_to_string(tmpctx, struct amount_sat, &amount),
			     tal_hex(tmpctx, wscript));
		tx->output_witscripts[n] =
			tal(tx->output_witscripts, struct witscript);
		tx->output_witscripts[n]->ptr =
			tal_dup_arr(tx->output_witscripts[n], u8,
				    wscript, tal_count(wscript), 0);
		n++;
	}

	/* BOLT #3:
	 *
	 * 6. If the `to_remote` amount is greater or equal to
	 *    `dust_limit_satoshis`, add a [`to_remote`
	 *    output](#to_remote-output).
	 */
	if (amount_msat_greater_eq_sat(other_pay, dust_limit)) {
		struct amount_sat amount = amount_msat_to_sat_round_down(other_pay);
		u8 *p2wpkh =
		    scriptpubkey_p2wpkh(tx, &keyset->other_payment_key);
		/* BOLT #3:
		 *
		 * #### `to_remote` Output
		 *
		 * This output sends funds to the other peer and thus is a simple
		 * P2WPKH to `remotepubkey`.
		 */
		int pos = bitcoin_tx_add_output(tx, p2wpkh, amount);
		assert(pos == n);
		(*htlcmap)[n] = direct_outputs ? dummy_to_remote : NULL;
		/* We don't assign cltvs[n]: if we use it, order doesn't matter.
		 * However, valgrind will warn us something wierd is happening */
		SUPERVERBOSE("# to-remote amount %s P2WPKH(%s)\n",
			     type_to_string(tmpctx, struct amount_sat,
					    &amount),
			     type_to_string(tmpctx, struct pubkey,
					    &keyset->other_payment_key));
		n++;
	}

	/* BOLT #2:
	 *
	 *  - MUST set `channel_reserve_satoshis` greater than or equal to
	 *    `dust_limit_satoshis`.
	 */
	/* This means there must be at least one output. */
	assert(n > 0);

	assert(n <= tx->wtx->outputs_allocation_len);
	tal_resize(htlcmap, n);

	/* BOLT #3:
	 *
	 * 7. Sort the outputs into [BIP 69+CLTV
	 *    order](#transaction-input-and-output-ordering)
	 */
	permute_outputs(tx, cltvs, (const void **)*htlcmap);

	/* BOLT #3:
	 *
	 * ## Commitment Transaction
	 *
	 * * version: 2
	 */
	assert(tx->wtx->version == 2);

	/* BOLT #3:
	 *
	 * * locktime: upper 8 bits are 0x20, lower 24 bits are the lower 24 bits of the obscured commitment number
	 */
	tx->wtx->locktime
		= (0x20000000 | (obscured_commitment_number & 0xFFFFFF));

	/* BOLT #3:
	 *
	 * * txin count: 1
	 *    * `txin[0]` outpoint: `txid` and `output_index` from
	 *      `funding_created` message
	 */
	/* BOLT #3:
	 *
	 *    * `txin[0]` sequence: upper 8 bits are 0x80, lower 24 bits are upper 24 bits of the obscured commitment number
	 */
	u32 sequence = (0x80000000 | ((obscured_commitment_number>>24) & 0xFFFFFF));
	bitcoin_tx_add_input(tx, funding_txid, funding_txout, sequence, funding, NULL);

	/* Identify the direct outputs (to_us, to_them). */
	if (direct_outputs != NULL) {
		direct_outputs[LOCAL] = direct_outputs[REMOTE] = NULL;
		for (size_t i = 0; i < tx->wtx->num_outputs; i++) {
			if ((*htlcmap)[i] == dummy_to_local) {
				(*htlcmap)[i] = NULL;
				direct_outputs[LOCAL] = tx->wtx->outputs + i;
			} else if ((*htlcmap)[i] == dummy_to_remote) {
				(*htlcmap)[i] = NULL;
				direct_outputs[REMOTE] = tx->wtx->outputs + i;
			}
		}
	}
	/* RGB
	 *
	 * Tweak public key with the commitment to the client-validated
	 * state for an output pointed by the current fee.
	 *
	 * 1. Request from the RGB plugin
	 *    - `cmt_blinding`: protocol-specific LNPBP-3 blinding factor
	 *    - `cmt_value`: 256-bit value of the client-validated state commitment
	 *
	 *    Both can be done through exchanging messages with plugin by calling
	 *    `wire_sync_write` followed by `wire_sync_read` to the main lightningd
	 *    with WIRE_RGB_STATUSUPDATE message
	 *
	 *    ISSUE: we do not have a file descriptor for the lightningd here
	 *
	 * 2. Take the output with index `(cmt_blinding + fee) mod num_outputs`
	 *   and tweak it's public key with `cmt_value`
	 *
	 * For this reason we need to extend plugin functionality to allow
	 * them to return data
	 */


	bitcoin_tx_finalize(tx);
	assert(bitcoin_tx_check(tx));

	return tx;
}
