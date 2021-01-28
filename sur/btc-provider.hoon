/-  *btc
|%
+$  host-info
  $:  api-url=@t
      connected=?
      =network
      block=@ud
      clients=(set ship)
  ==
+$  command
  $%  [%set-credentials api-url=@t =network] 
      [%whitelist-clients clients=(set ship)]
  ==
+$  action
  $%  [%address-info =address]
      [%tx-info txid=hexb]
      [%raw-tx txid=hexb]
      [%broadcast-tx rawtx=hexb]
      [%ping ~]
  ==
+$  result
  $%  [%address-info =address utxos=(set utxo) used=? block=@ud]
      [%tx-info =info:tx]
      [%raw-tx txid=hexb rawtx=hexb]
      [%broadcast-tx txid=hexb broadcast=? included=?]
  ==
+$  error
  $%  [%not-connected status=@ud]
      [%bad-request status=@ud]
      [%no-auth status=@ud]
      [%rpc-error ~]
  ==
+$  update  (each result error)
+$  status
  $%  [%connected block=@ud fee=sats]
      [%new-block block=@ud fee=sats blockhash=hexb blockfilter=hexb]
      [%disconnected ~]
  ==
::
++  rpc-types
  |%
  +$  action
    $%  [%get-address-info =address]
        [%get-tx-vals txid=hexb]
        [%get-raw-tx txid=hexb]
        [%broadcast-tx rawtx=hexb]
        [%get-block-count ~]
        [%get-block-info ~]
    ==
  ::
  +$  result
    $%  [%get-address-info =address utxos=(set utxo) used=? block=@ud]
        [%get-tx-vals =info:tx]
        [%get-raw-tx txid=hexb rawtx=hexb]
        [%create-raw-tx rawtx=hexb]
        [%broadcast-tx txid=hexb broadcast=? included=?]
        [%get-block-count block=@ud]
        [%get-block-info block=@ud fee=sats blockhash=hexb blockfilter=hexb]

    ==
  --
--
::
