DROP FUNCTION IF EXISTS test_given;
CREATE FUNCTION test_given()
    RETURNS void
    LANGUAGE 'plpgsql'
VOLATILE
AS
$BODY$
BEGIN
    PERFORM hive.app_create_context( 'context' );

    INSERT INTO hive.operation_types
    VALUES (0, 'OP 0', FALSE )
        , ( 1, 'OP 1', FALSE )
        , ( 2, 'OP 2', FALSE )
        , ( 3, 'OP 3', TRUE )
    ;

    INSERT INTO hive.fork( id, block_num, time_of_fork)
    VALUES ( 2, 6, '2020-06-22 19:10:25-07'::timestamp ),
           ( 3, 7, '2020-06-22 19:10:25-07'::timestamp );

    INSERT INTO hive.blocks
    VALUES
    ( 1, '\xBADD10', '\xCAFE10', '2016-06-22 19:10:21-07'::timestamp )
         , ( 2, '\xBADD20', '\xCAFE20', '2016-06-22 19:10:22-07'::timestamp )
         , ( 3, '\xBADD30', '\xCAFE30', '2016-06-22 19:10:23-07'::timestamp )
         , ( 4, '\xBADD40', '\xCAFE40', '2016-06-22 19:10:24-07'::timestamp )
    ;

    INSERT INTO hive.transactions
    VALUES
           ( 1, 0::SMALLINT, '\xDEED10', 101, 100, '2016-06-22 19:10:21-07'::timestamp, '\xBEEF' )
         , ( 2, 0::SMALLINT, '\xDEED20', 101, 100, '2016-06-22 19:10:22-07'::timestamp, '\xBEEF' )
         , ( 3, 0::SMALLINT, '\xDEED30', 101, 100, '2016-06-22 19:10:23-07'::timestamp, '\xBEEF' )
         , ( 4, 0::SMALLINT, '\xDEED40', 101, 100, '2016-06-22 19:10:24-07'::timestamp, '\xBEEF' )
    ;

    INSERT INTO hive.operations
    VALUES
          ( 1, 1, 0, 0, 1, 'ZERO OPERATION' )
        , ( 2, 2, 0, 0, 1, 'ONE OPERATION' )
        , ( 3, 3, 0, 0, 1, 'TWO OPERATION' )
        , ( 4, 4, 0, 0, 1, 'THREE OPERATION' )
    ;

    INSERT INTO hive.blocks_reversible
    VALUES
           ( 4, '\xBADD40', '\xCAFE40', '2016-06-22 19:10:25-07'::timestamp, 1 )
         , ( 5, '\xBADD5A', '\xCAFE5A', '2016-06-22 19:10:55-07'::timestamp, 1 )
         , ( 6, '\xBADD60', '\xCAFE60', '2016-06-22 19:10:26-07'::timestamp, 1 )
         , ( 7, '\xBADD71', '\xCAFE71', '2016-06-22 19:10:27-07'::timestamp, 1 )
         , ( 7, '\xBADD70', '\xCAFE70', '2016-06-22 19:10:27-07'::timestamp, 2 )
         , ( 8, '\xBADD80', '\xCAFE80', '2016-06-22 19:10:28-07'::timestamp, 2 )
         , ( 9, '\xBADD90', '\xCAFE90', '2016-06-22 19:10:29-07'::timestamp, 2 )
         , ( 8, '\xBADD80', '\xCAFE80', '2016-06-22 19:10:30-07'::timestamp, 3 )
         , ( 9, '\xBADD90', '\xCAFE90', '2016-06-22 19:10:31-07'::timestamp, 3 )
         , ( 10, '\xBADD1A', '\xCAFE1A', '2016-06-22 19:10:32-07'::timestamp, 3 )
    ;

    INSERT INTO hive.transactions_reversible
    VALUES
       ( 4, 0::SMALLINT, '\xDEED40', 101, 100, '2016-06-22 19:10:24-07'::timestamp, '\xBEEF',  1 )
     , ( 5, 0::SMALLINT, '\xDEED55', 101, 100, '2016-06-22 19:10:25-07'::timestamp, '\xBEEF',  1 )
     , ( 6, 0::SMALLINT, '\xDEED60', 101, 100, '2016-06-22 19:10:26-07'::timestamp, '\xBEEF',  1 )
     , ( 7, 0::SMALLINT, '\xDEED70', 101, 100, '2016-06-22 19:10:27-07'::timestamp, '\xBEEF',  2 )
     , ( 8, 0::SMALLINT, '\xDEED80', 101, 100, '2016-06-22 19:10:28-07'::timestamp, '\xBEEF',  2 )
     , ( 9, 0::SMALLINT, '\xDEED90', 101, 100, '2016-06-22 19:10:29-07'::timestamp, '\xBEEF',  2 )
     , ( 8, 0::SMALLINT, '\xDEED88', 101, 100, '2016-06-22 19:10:28-07'::timestamp, '\xBEEF',  3 )
     , ( 9, 0::SMALLINT, '\xDEED99', 101, 100, '2016-06-22 19:10:29-07'::timestamp, '\xBEEF',  3 )
     , ( 10, 0::SMALLINT, '\xDEED11', 101, 100, '2016-06-22 19:10:30-07'::timestamp, '\xBEEF', 3 )
    ;

    INSERT INTO hive.operations_reversible
    VALUES
           ( 4, 4, 0, 0, 1, 'THREE OPERATION', 1 )
         , ( 5, 5, 0, 0, 1, 'FIVEFIVE OPERATION', 1 )
         , ( 6, 6, 0, 0, 1, 'SIX OPERATION', 1 )
         , ( 7, 7, 0, 0, 1, 'SEVEN0 OPERATION', 1 ) -- must be abandon because of fork2
         , ( 8, 7, 0, 1, 1, 'SEVEN01 OPERATION', 1 ) -- must be abandon because of fork2
         , ( 9, 7, 0, 2, 1, 'SEVEN02 OPERATION', 1 ) -- must be abandon because of fork2
         , ( 7, 7, 0, 0, 1, 'SEVEN2 OPERATION', 2 )
         , ( 8, 7, 0, 1, 1, 'SEVEN21 OPERATION', 2 )
         , ( 9, 8, 0, 0, 1, 'EAIGHT2 OPERATION', 2 )
         , ( 10, 9, 0, 0, 1, 'NINE2 OPERATION', 2 )
         , ( 8, 8, 0, 0, 1, 'EIGHT3 OPERATION', 3 )
         , ( 9, 9, 0, 0, 1, 'NINE3 OPERATION', 3 )
         , ( 10, 10, 0, 0, 1, 'TEN OPERATION', 3 )
    ;

    UPDATE hive.contexts SET fork_id = 2, irreversible_block = 4, current_block_num = 8;
END;
$BODY$
;

DROP FUNCTION IF EXISTS test_when;
CREATE FUNCTION test_when()
    RETURNS void
    LANGUAGE 'plpgsql'
VOLATILE
AS
$BODY$
BEGIN
    --NOTHING TODO HERE
END
$BODY$
;

DROP FUNCTION IF EXISTS test_then;
CREATE FUNCTION test_then()
    RETURNS void
    LANGUAGE 'plpgsql'
STABLE
AS
$BODY$
BEGIN
    ASSERT EXISTS ( SELECT FROM information_schema.tables WHERE table_schema='hive' AND table_name='context_operations_view' ), 'No context transactions view';

    ASSERT NOT EXISTS (
        SELECT * FROM hive.context_operations_view
        EXCEPT SELECT * FROM ( VALUES
              ( 1, 1, 0, 0, 1, 'ZERO OPERATION' )
            , ( 2, 2, 0, 0, 1, 'ONE OPERATION' )
            , ( 3, 3, 0, 0, 1, 'TWO OPERATION' )
            , ( 4, 4, 0, 0, 1, 'THREE OPERATION' )
        ) as pattern
    ) , 'Unexpected rows in the view';

    ASSERT NOT EXISTS (
        SELECT * FROM ( VALUES
              ( 1, 1, 0, 0, 1, 'ZERO OPERATION' )
            , ( 2, 2, 0, 0, 1, 'ONE OPERATION' )
            , ( 3, 3, 0, 0, 1, 'TWO OPERATION' )
            , ( 4, 4, 0, 0, 1, 'THREE OPERATION' )
        ) as pattern
        EXCEPT SELECT * FROM hive.context_operations_view
    ) , 'Unexpected rows in the view2';
END;
$BODY$
;




