# subscribers

A couple of trivial samples how to organize multithreaded delivery of notifications to a set of
subscribers. One can think it could be be a super small brother of boost::signals2 library. A "notification"
is just a call to previously provided std::function<void()> functor. Any parameter handling stuff is
not a purpose of this code so far. The only goal of it is to organize subscription/unsubscription
and notification delivery in multithreaded environment. The second sample allows to unsubscribe a
consumer while being called from the consumer's callback.
